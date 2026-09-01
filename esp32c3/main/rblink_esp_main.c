#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "rblink_link_protocol.h"
#include "rblink_provisioning.h"

#define RBLINK_SPI_HOST       SPI2_HOST
#define RBLINK_GPIO_SCLK      GPIO_NUM_4
#define RBLINK_GPIO_MOSI      GPIO_NUM_5
#define RBLINK_GPIO_MISO      GPIO_NUM_6
#define RBLINK_GPIO_CS        GPIO_NUM_7
#define RBLINK_GPIO_READY     GPIO_NUM_10
#define RBLINK_GPIO_LED_LINK  GPIO_NUM_0
#define RBLINK_GPIO_LED_DATA  GPIO_NUM_1

#define RBLINK_TCP_TASK_STACK 6144U
#define RBLINK_DISCOVERY_TASK_STACK 4096U
#define RBLINK_SPI_TASK_STACK 4096U
#define RBLINK_LED_TASK_STACK  2048U
#define RBLINK_ACTIVITY_MS     60U
#define RBLINK_LINK_BLINK_MS   500U
#define RBLINK_DISCOVERY_PORT  3241U

static const char *TAG = "rblink";
static QueueHandle_t s_net_to_stm32;
static QueueHandle_t s_stm32_to_net;
static SemaphoreHandle_t s_web_exchange_mutex;
static TimerHandle_t s_activity_timer;
static volatile bool s_tcp_client_connected;
static volatile bool s_web_exchange_active;
static volatile uint32_t s_spi_transactions_completed;
static volatile uint32_t s_tcp_frames_queued_to_spi;
static volatile uint32_t s_tcp_frames_transmitted_to_spi;
static volatile uint32_t s_spi_valid_idle_frames_received;
static volatile uint32_t s_spi_valid_nonidle_frames_received;
static volatile uint32_t s_spi_invalid_frames_received;
static volatile uint32_t s_spi_frames_queued_to_tcp;
static bool rblink_make_local_control_response(const rblink_frame_t *request,
                                               rblink_frame_t *response);
static bool rblink_web_debug_exchange(const uint8_t *request_bytes,
                                      uint8_t *response_bytes);

static void rblink_activity_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    gpio_set_level(RBLINK_GPIO_LED_DATA, 0);
}

/* Extend the visible activity pulse for every valid SPI or TCP transfer.
 * The one-shot software timer avoids delaying either communication task. */
static void rblink_activity_kick(void)
{
    gpio_set_level(RBLINK_GPIO_LED_DATA, 1);
    if (xTimerReset(s_activity_timer, 0) != pdPASS) {
        /* Never leave the activity LED permanently on if the timer command
           queue is momentarily full. */
        gpio_set_level(RBLINK_GPIO_LED_DATA, 0);
    }
}

/* Slow blink means that the ESP firmware is alive and waiting for a desktop
 * client; a steady light means that the wireless debug link is occupied. */
static void rblink_led_task(void *argument)
{
    bool blink_level = false;

    (void)argument;
    for (;;) {
        if (s_tcp_client_connected) {
            gpio_set_level(RBLINK_GPIO_LED_LINK, 1);
        } else {
            blink_level = !blink_level;
            gpio_set_level(RBLINK_GPIO_LED_LINK, blink_level ? 1 : 0);
        }
        vTaskDelay(pdMS_TO_TICKS(RBLINK_LINK_BLINK_MS));
    }
}

/* READY is an electrical transaction-ready handshake, not a data-pending flag. */
static void IRAM_ATTR rblink_spi_post_setup(spi_slave_transaction_t *transaction)
{
    (void)transaction;
    gpio_set_level(RBLINK_GPIO_READY, 1);
}

static void IRAM_ATTR rblink_spi_post_trans(spi_slave_transaction_t *transaction)
{
    (void)transaction;
    gpio_set_level(RBLINK_GPIO_READY, 0);
}

static void rblink_led_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (UINT64_C(1) << RBLINK_GPIO_LED_LINK) |
                        (UINT64_C(1) << RBLINK_GPIO_LED_DATA) |
                        (UINT64_C(1) << RBLINK_GPIO_READY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(RBLINK_GPIO_LED_LINK, 0);
    gpio_set_level(RBLINK_GPIO_LED_DATA, 0);
    gpio_set_level(RBLINK_GPIO_READY, 0);
}

static void rblink_spi_init(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = RBLINK_GPIO_MOSI,
        .miso_io_num = RBLINK_GPIO_MISO,
        .sclk_io_num = RBLINK_GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RBLINK_FRAME_SIZE,
    };
    const spi_slave_interface_config_t slave_config = {
        .spics_io_num = RBLINK_GPIO_CS,
        .flags = 0,
        .queue_size = 2,
        .mode = 0,
        .post_setup_cb = rblink_spi_post_setup,
        .post_trans_cb = rblink_spi_post_trans,
    };

    ESP_ERROR_CHECK(spi_slave_initialize(RBLINK_SPI_HOST, &bus_config,
                                         &slave_config, SPI_DMA_CH_AUTO));
}

static void rblink_spi_task(void *argument)
{
    rblink_frame_t *tx_frame;
    rblink_frame_t *rx_frame;
    uint32_t idle_sequence = 0U;

    (void)argument;
    /* MALLOC_CAP_DMA allocations are word aligned as required by SPI DMA. */
    tx_frame = heap_caps_malloc(sizeof(*tx_frame), MALLOC_CAP_DMA);
    rx_frame = heap_caps_malloc(sizeof(*rx_frame), MALLOC_CAP_DMA);
    if ((tx_frame == NULL) || (rx_frame == NULL)) {
        ESP_LOGE(TAG, "failed to allocate SPI DMA buffers");
        abort();
    }

    for (;;) {
        spi_slave_transaction_t transaction = {0};
        spi_slave_transaction_t *completed = NULL;
        bool transmitting_network_frame;

        transmitting_network_frame =
            xQueueReceive(s_net_to_stm32, tx_frame, 0) == pdTRUE;
        if (!transmitting_network_frame) {
            rblink_frame_make_idle(tx_frame, idle_sequence++);
        }
        memset(rx_frame, 0, sizeof(*rx_frame));

        transaction.length = RBLINK_FRAME_SIZE * 8U;
        transaction.tx_buffer = tx_frame;
        transaction.rx_buffer = rx_frame;

        ESP_ERROR_CHECK(spi_slave_queue_trans(RBLINK_SPI_HOST, &transaction,
                                              portMAX_DELAY));
        ESP_ERROR_CHECK(spi_slave_get_trans_result(RBLINK_SPI_HOST, &completed,
                                                   portMAX_DELAY));
        ++s_spi_transactions_completed;
        if (transmitting_network_frame) {
            ++s_tcp_frames_transmitted_to_spi;
        }

        if (completed->trans_len != (RBLINK_FRAME_SIZE * 8U)) {
            ++s_spi_invalid_frames_received;
            continue;
        }
        if (!rblink_frame_is_valid(rx_frame)) {
            ++s_spi_invalid_frames_received;
            continue;
        }
        if (rx_frame->channel == RBLINK_CHANNEL_IDLE) {
            ++s_spi_valid_idle_frames_received;
        } else {
            rblink_frame_t local_response;
            QueueHandle_t destination = s_stm32_to_net;
            const rblink_frame_t *queued_frame = rx_frame;
            ++s_spi_valid_nonidle_frames_received;
            if (rblink_make_local_control_response(rx_frame, &local_response)) {
                /* STM32-originated control commands terminate on ESP and the
                 * response travels back in the next SPI transaction. */
                destination = s_net_to_stm32;
                queued_frame = &local_response;
            }
            if (xQueueSend(destination, queued_frame, 0) != pdTRUE) {
                ESP_LOGW(TAG, "network TX queue full; dropping sequence %" PRIu32,
                         rx_frame->sequence);
            } else if (destination == s_stm32_to_net) {
                ++s_spi_frames_queued_to_tcp;
            }
            rblink_activity_kick();
        }
    }
}

static int rblink_create_server_socket(void)
{
    const int reuse = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_RBLINK_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    if (socket_fd < 0) {
        return -1;
    }
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if ((bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) ||
        (listen(socket_fd, 1) != 0)) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static bool rblink_send_frame(int socket_fd, const rblink_frame_t *frame)
{
    size_t sent = 0U;

    while (sent < sizeof(*frame)) {
        int result = send(socket_fd, (const uint8_t *)frame + sent,
                          sizeof(*frame) - sent, 0);
        if (result <= 0) {
            return false;
        }
        sent += (size_t)result;
    }
    return true;
}

static bool rblink_make_local_control_response(const rblink_frame_t *request,
                                               rblink_frame_t *response)
{
    if ((request->channel != RBLINK_CHANNEL_CONTROL) ||
        ((request->flags & RBLINK_FLAG_RESPONSE) != 0U) ||
        (request->payload_length == 0U)) {
        return false;
    }

    memset(response, 0, sizeof(*response));
    response->channel = RBLINK_CHANNEL_CONTROL;
    response->flags = RBLINK_FLAG_RESPONSE;
    response->sequence = request->sequence;
    if (request->payload[0] == RBLINK_CONTROL_PING) {
        response->payload_length = request->payload_length;
        memcpy(response->payload, request->payload, request->payload_length);
    } else if (request->payload[0] == RBLINK_CONTROL_GET_INFO) {
        uint8_t info[] = {
            RBLINK_CONTROL_GET_INFO, 0U, RBLINK_PROTOCOL_VERSION,
            0U, 2U, 1U, /* ESP firmware 0.2.1: UDP LAN discovery */
            0x3FU, 0U,  /* AP, SPI, TCP, STA, STM32 provision, AP web */
            0U, 0U, 0U, 0U, /* TCP frames queued to SPI */
            0U, 0U, 0U, 0U, /* TCP frames transmitted over SPI */
            0U, 0U, 0U, 0U, /* completed SPI transactions */
            0U, 0U, 0U, 0U, /* valid idle frames received from STM32 */
            0U, 0U, 0U, 0U, /* valid non-idle frames received from STM32 */
            0U, 0U, 0U, 0U, /* invalid/short frames received from STM32 */
            0U, 0U, 0U, 0U, /* SPI frames queued back to TCP */
        };
        const uint32_t counters[] = {
            s_tcp_frames_queued_to_spi,
            s_tcp_frames_transmitted_to_spi,
            s_spi_transactions_completed,
            s_spi_valid_idle_frames_received,
            s_spi_valid_nonidle_frames_received,
            s_spi_invalid_frames_received,
            s_spi_frames_queued_to_tcp,
        };
        memcpy(&info[8], counters, sizeof(counters));
        response->payload_length = sizeof(info);
        memcpy(response->payload, info, sizeof(info));
    } else {
        uint16_t response_length = 0U;
        if (!rblink_provisioning_command(request->payload,
                                         request->payload_length,
                                         response->payload,
                                         &response_length)) {
            return false;
        }
        /* Assign through an aligned local because frame headers are packed. */
        response->payload_length = response_length;
    }
    rblink_frame_finalize(response);
    return true;
}

static void rblink_tcp_task(void *argument)
{
    rblink_frame_t rx_frame;
    rblink_frame_t tx_frame;
    size_t received = 0U;
    int server_fd;
    int client_fd = -1;

    (void)argument;
    while ((server_fd = rblink_create_server_socket()) < 0) {
        ESP_LOGE(TAG, "failed to create TCP server: errno=%d; retrying", errno);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (;;) {
        fd_set read_fds;
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 20000};
        int max_fd = server_fd;

        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        if (client_fd >= 0) {
            FD_SET(client_fd, &read_fds);
            max_fd = client_fd > max_fd ? client_fd : max_fd;
        }

        int selected = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (selected < 0) {
            ESP_LOGW(TAG, "select failed: errno=%d", errno);
            continue;
        }

        if (FD_ISSET(server_fd, &read_fds)) {
            int new_client = accept(server_fd, NULL, NULL);
            if (new_client >= 0) {
                const struct timeval send_timeout = {.tv_sec = 2, .tv_usec = 0};
                if ((client_fd >= 0) || s_web_exchange_active) {
                    /* Keep the active debugging session stable. A second PC
                     * must retry after the current owner disconnects. */
                    close(new_client);
                    ESP_LOGW(TAG, "rejected second TCP client");
                    new_client = -1;
                }
                if (new_client >= 0) {
                    setsockopt(new_client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                               sizeof(send_timeout));
                    xQueueReset(s_net_to_stm32);
                    xQueueReset(s_stm32_to_net);
                    client_fd = new_client;
                    received = 0U;
                    s_tcp_client_connected = true;
                    ESP_LOGI(TAG, "TCP client connected");
                }
            }
        }

        if ((client_fd >= 0) && FD_ISSET(client_fd, &read_fds)) {
            int result = recv(client_fd, (uint8_t *)&rx_frame + received,
                              sizeof(rx_frame) - received, 0);
            if (result <= 0) {
                close(client_fd);
                client_fd = -1;
                received = 0U;
                s_tcp_client_connected = false;
                ESP_LOGI(TAG, "TCP client disconnected");
            } else {
                rblink_activity_kick();
                received += (size_t)result;
                if (received == sizeof(rx_frame)) {
                    if (!rblink_frame_is_valid(&rx_frame) ||
                        (rx_frame.channel == RBLINK_CHANNEL_IDLE)) {
                        ESP_LOGW(TAG, "discarding invalid TCP frame");
                    } else if (rx_frame.channel == RBLINK_CHANNEL_CONTROL) {
                        /* Network clients may run health checks, but Wi-Fi
                         * credentials are accepted only from STM32 over SPI. */
                        if (((rx_frame.payload[0] == RBLINK_CONTROL_PING) ||
                             (rx_frame.payload[0] == RBLINK_CONTROL_GET_INFO)) &&
                            rblink_make_local_control_response(&rx_frame,
                                                               &tx_frame)) {
                            if (xQueueSend(s_stm32_to_net, &tx_frame, 0) != pdTRUE) {
                                ESP_LOGW(TAG, "control response queue full");
                            }
                        } else {
                            ESP_LOGW(TAG, "rejected privileged TCP control command");
                        }
                    } else if (xQueueSend(s_net_to_stm32, &rx_frame, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "SPI TX queue full; dropping TCP frame");
                    } else {
                        ++s_tcp_frames_queued_to_spi;
                    }
                    received = 0U;
                }
            }
        }

        if (client_fd >= 0) {
            while (xQueueReceive(s_stm32_to_net, &tx_frame, 0) == pdTRUE) {
                if (!rblink_send_frame(client_fd, &tx_frame)) {
                    close(client_fd);
                    client_fd = -1;
                    s_tcp_client_connected = false;
                    break;
                }
                rblink_activity_kick();
            }
        } else if (!s_web_exchange_active) {
            /* Stale responses cannot be matched after the next PC reconnect. */
            while (xQueueReceive(s_stm32_to_net, &tx_frame, 0) == pdTRUE) {
            }
        }
    }
}

static void rblink_discovery_task(void *argument)
{
    rblink_frame_t request;
    rblink_frame_t response;
    struct sockaddr_storage peer;
    socklen_t peer_length;
    int socket_fd;

    (void)argument;
    for (;;) {
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(RBLINK_DISCOVERY_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if ((socket_fd < 0) ||
            (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0)) {
            ESP_LOGE(TAG, "failed to create UDP discovery socket: errno=%d; retrying", errno);
            if (socket_fd >= 0) {
                close(socket_fd);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "UDP discovery listening on port %u", RBLINK_DISCOVERY_PORT);
        for (;;) {
            peer_length = sizeof(peer);
            int received = recvfrom(socket_fd, &request, sizeof(request), 0,
                                    (struct sockaddr *)&peer, &peer_length);
            if (received < 0) {
                ESP_LOGW(TAG, "UDP discovery receive failed: errno=%d", errno);
                break;
            }
            if ((received == sizeof(request)) &&
                rblink_frame_is_valid(&request) &&
                (request.channel == RBLINK_CHANNEL_CONTROL) &&
                (request.payload_length == 1U) &&
                (request.payload[0] == RBLINK_CONTROL_GET_INFO) &&
                rblink_make_local_control_response(&request, &response)) {
                (void)sendto(socket_fd, &response, sizeof(response), 0,
                             (struct sockaddr *)&peer, peer_length);
            }
        }
        close(socket_fd);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static bool rblink_web_debug_exchange(const uint8_t *request_bytes,
                                      uint8_t *response_bytes)
{
    const rblink_frame_t *request = (const rblink_frame_t *)request_bytes;
    rblink_frame_t *response = (rblink_frame_t *)response_bytes;
    rblink_frame_t queued;
    bool success = false;

    if ((request_bytes == NULL) || (response_bytes == NULL) ||
        !rblink_frame_is_valid(request) ||
        (request->channel == RBLINK_CHANNEL_IDLE) ||
        s_tcp_client_connected ||
        (xSemaphoreTake(s_web_exchange_mutex, 0) != pdTRUE)) {
        return false;
    }
    s_web_exchange_active = true;
    while (xQueueReceive(s_stm32_to_net, &queued, 0) == pdTRUE) {
    }
    if (rblink_make_local_control_response(request, response)) {
        success = true;
    } else if (xQueueSend(s_net_to_stm32, request, pdMS_TO_TICKS(100)) == pdTRUE) {
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
            if ((xQueueReceive(s_stm32_to_net, &queued, pdMS_TO_TICKS(20)) == pdTRUE) &&
                (queued.sequence == request->sequence) &&
                (queued.channel == request->channel) &&
                ((queued.flags & RBLINK_FLAG_RESPONSE) != 0U)) {
                memcpy(response, &queued, sizeof(*response));
                success = true;
                break;
            }
        }
    }
    s_web_exchange_active = false;
    xSemaphoreGive(s_web_exchange_mutex);
    return success;
}

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    BaseType_t task_result;

    if ((nvs_result == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }

    rblink_led_init();
    s_tcp_client_connected = false;
    s_activity_timer = xTimerCreate("activity_led",
                                    pdMS_TO_TICKS(RBLINK_ACTIVITY_MS),
                                    pdFALSE, NULL,
                                    rblink_activity_timer_callback);
    if (s_activity_timer == NULL) {
        ESP_LOGE(TAG, "failed to create activity LED timer");
        abort();
    }
    s_net_to_stm32 = xQueueCreate(CONFIG_RBLINK_SPI_QUEUE_DEPTH,
                                  sizeof(rblink_frame_t));
    s_stm32_to_net = xQueueCreate(CONFIG_RBLINK_SPI_QUEUE_DEPTH,
                                  sizeof(rblink_frame_t));
    if ((s_net_to_stm32 == NULL) || (s_stm32_to_net == NULL)) {
        ESP_LOGE(TAG, "failed to create frame queues");
        abort();
    }

    s_web_exchange_mutex = xSemaphoreCreateMutex();
    if (s_web_exchange_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create Web debug mutex");
        abort();
    }
    rblink_provisioning_init(rblink_web_debug_exchange);
    rblink_spi_init();
    task_result = xTaskCreate(rblink_led_task, "rblink_led",
                              RBLINK_LED_TASK_STACK, NULL, 3, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_result = xTaskCreate(rblink_spi_task, "rblink_spi",
                              RBLINK_SPI_TASK_STACK, NULL, 10, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_result = xTaskCreate(rblink_tcp_task, "rblink_tcp",
                              RBLINK_TCP_TASK_STACK, NULL, 8, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_result = xTaskCreate(rblink_discovery_task, "rblink_discovery",
                              RBLINK_DISCOVERY_TASK_STACK, NULL, 7, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
