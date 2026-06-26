idf_component_register(SRCS "net.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_wifi_remote esp_hosted esp_netif esp_event
                                nvs_flash esp_http_client esp-tls)
