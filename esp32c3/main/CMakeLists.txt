idf_component_register(
    SRCS
        "rblink_esp_main.c"
        "rblink_link_protocol.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_event
        esp_netif
        esp_wifi
        lwip
        nvs_flash
)
