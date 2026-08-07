idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server
                    PRIV_REQUIRES json freertos log esp_http_client esp_partition app_update cjson)
