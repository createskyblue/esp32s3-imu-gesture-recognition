idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES littlefs esp_http_server
                    PRIV_REQUIRES freertos log esp_ota_service esp_service esp_partition app_update cjson)
