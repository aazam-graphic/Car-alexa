idf_component_register(SRCS "c3_node_main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_wifi esp_event esp_netif nvs_flash esp_timer)