idf_component_register(
    SRCS "json_http.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES cjson
)
