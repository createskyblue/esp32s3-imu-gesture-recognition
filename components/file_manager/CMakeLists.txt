idf_component_register(SRCS "file_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server
                    PRIV_REQUIRES json littlefs fatfs cjson log heap)
