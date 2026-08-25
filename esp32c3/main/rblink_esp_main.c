#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "rblink_link_protocol.h"

#define RBLINK_SPI_HOST       SPI2_HOST
#define RBLINK_GPIO_SCLK      GPIO_NUM_4
#define RBLINK_GPIO_MOSI      GPIO_NUM_5
#define RBLINK_GPIO_MISO      GPIO_NUM_6
#define RBLINK_GPIO_CS        GPIO_NUM_7
#define RBLINK_GPIO_READY     GPIO_NUM_10
#define RBLINK_GPIO_LED_LINK  GPIO_NUM_0
#define RBLINK_GPIO_LED_DATA  GPIO_NUM_1

#define RBLINK_TCP_TASK_STACK 6144U
#define RBLINK_SPI_TASK_STACK 4096U
#define RBLINK_WIFI_CHANNEL   1U
#define RBLINK_MAX_CLIENTS    1U

static const char *TAG = "rblink";
static QueueHandle_t s_net_to_stm32;
static QueueHandle_t s_stm32_to_net;

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

static void rblink_wifi_start(void)
{
    uint8_t mac[6];
    char ssid[33];
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap_config = {0};

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "failed to create default SoftAP network interface");
        abort();
    }

    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X%02X",
             CONFIG_RBLINK_AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    strlcpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, CONFIG_RBLINK_AP_PASSWORD,
            sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = (uint8_t)strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = RBLINK_WIFI_CHANNEL;
    ap_config.ap.max_connection = RBLINK_MAX_CLIENTS;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP %s started; TCP port %d", ssid,
             CONFIG_RBLINK_TCP_PORT);
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

        if (xQueueReceive(s_net_to_stm32, tx_frame, 0) != pdTRUE) {
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

        if ((completed->trans_len == (RBLINK_FRAME_SIZE * 8U)) &&
            rblink_frame_is_valid(rx_frame) &&
            (rx_frame->channel != RBLINK_CHANNEL_IDLE)) {
            if (xQueueSend(s_stm32_to_net, rx_frame, 0) != pdTRUE) {
                ESP_LOGW(TAG, "network TX queue full; dropping sequence %" PRIu32,
                         rx_frame->sequence);
            }
            gpio_set_level(RBLINK_GPIO_LED_DATA, 1);
            vTaskDelay(pdMS_TO_TICKS(1));
            gpio_set_level(RBLINK_GPIO_LED_DATA, 0);
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

static bool rblink_handle_local_control(const rblink_frame_t *request)
{
    rblink_frame_t response = {0};

    if ((request->channel != RBLINK_CHANNEL_CONTROL) ||
        ((request->flags & RBLINK_FLAG_RESPONSE) != 0U) ||
        (request->payload_length == 0U)) {
        return false;
    }

    response.channel = RBLINK_CHANNEL_CONTROL;
    response.flags = RBLINK_FLAG_RESPONSE;
    response.sequence = request->sequence;
    if (request->payload[0] == RBLINK_CONTROL_PING) {
        response.payload_length = request->payload_length;
        memcpy(response.payload, request->payload, request->payload_length);
    } else if (request->payload[0] == RBLINK_CONTROL_GET_INFO) {
        static const uint8_t info[] = {
            RBLINK_CONTROL_GET_INFO, 0U, RBLINK_PROTOCOL_VERSION,
            0U, 1U, 0U, /* ESP firmware 0.1.0 */
            0x07U, 0U,  /* bit0 SoftAP, bit1 SPI, bit2 TCP */
        };
        response.payload_length = sizeof(info);
        memcpy(response.payload, info, sizeof(info));
    } else {
        return false;
    }

    rblink_frame_finalize(&response);
    if (xQueueSend(s_stm32_to_net, &response, 0) != pdTRUE) {
        ESP_LOGW(TAG, "control response queue full");
    }
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
    server_fd = rblink_create_server_socket();
    if (server_fd < 0) {
        ESP_LOGE(TAG, "failed to create TCP server: errno=%d", errno);
        vTaskDelete(NULL);
        return;
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
                if (client_fd >= 0) {
                    close(client_fd);
                }
                setsockopt(new_client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                           sizeof(send_timeout));
                xQueueReset(s_net_to_stm32);
                xQueueReset(s_stm32_to_net);
                client_fd = new_client;
                received = 0U;
                gpio_set_level(RBLINK_GPIO_LED_LINK, 1);
                ESP_LOGI(TAG, "TCP client connected");
            }
        }

        if ((client_fd >= 0) && FD_ISSET(client_fd, &read_fds)) {
            int result = recv(client_fd, (uint8_t *)&rx_frame + received,
                              sizeof(rx_frame) - received, 0);
            if (result <= 0) {
                close(client_fd);
                client_fd = -1;
                received = 0U;
                gpio_set_level(RBLINK_GPIO_LED_LINK, 0);
                ESP_LOGI(TAG, "TCP client disconnected");
            } else {
                received += (size_t)result;
                if (received == sizeof(rx_frame)) {
                    if (!rblink_frame_is_valid(&rx_frame) ||
                        (rx_frame.channel == RBLINK_CHANNEL_IDLE)) {
                        ESP_LOGW(TAG, "discarding invalid TCP frame");
                    } else if (rblink_handle_local_control(&rx_frame)) {
                        /* Control health checks terminate on the ESP itself. */
                    } else if (xQueueSend(s_net_to_stm32, &rx_frame, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "SPI TX queue full; dropping TCP frame");
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
                    gpio_set_level(RBLINK_GPIO_LED_LINK, 0);
                    break;
                }
            }
        } else {
            /* Stale responses cannot be matched after the next PC reconnect. */
            while (xQueueReceive(s_stm32_to_net, &tx_frame, 0) == pdTRUE) {
            }
        }
    }
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
    s_net_to_stm32 = xQueueCreate(CONFIG_RBLINK_SPI_QUEUE_DEPTH,
                                  sizeof(rblink_frame_t));
    s_stm32_to_net = xQueueCreate(CONFIG_RBLINK_SPI_QUEUE_DEPTH,
                                  sizeof(rblink_frame_t));
    if ((s_net_to_stm32 == NULL) || (s_stm32_to_net == NULL)) {
        ESP_LOGE(TAG, "failed to create frame queues");
        abort();
    }

    rblink_wifi_start();
    rblink_spi_init();
    task_result = xTaskCreate(rblink_spi_task, "rblink_spi",
                              RBLINK_SPI_TASK_STACK, NULL, 10, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_result = xTaskCreate(rblink_tcp_task, "rblink_tcp",
                              RBLINK_TCP_TASK_STACK, NULL, 8, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
