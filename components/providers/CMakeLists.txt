idf_component_register(SRCS "oura.cpp" "withings.cpp" "weather.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client esp-tls json model nvs_flash)
