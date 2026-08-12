set(HOST_SRCS "")
if(CONFIG_BLE_ENABLED)
    list(APPEND HOST_SRCS "ble_host.c")
endif()

idf_component_register(SRCS "${HOST_SRCS}"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash log freertos)
