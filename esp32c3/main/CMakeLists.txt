idf_component_register(
    SRCS
        "rblink_esp_main.c"
        "rblink_link_protocol.c"
        "rblink_provisioning.c"
    INCLUDE_DIRS "."
    EMBED_FILES "rb_logo.png" "index.html"
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        json
        lwip
        nvs_flash
)
