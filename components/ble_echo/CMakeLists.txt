set(ECHO_SRCS "")
if(CONFIG_BLE_ENABLED)
    list(APPEND ECHO_SRCS "ble_echo.c")
endif()

idf_component_register(SRCS "${ECHO_SRCS}"
    INCLUDE_DIRS "."
    REQUIRES ble_host
    PRIV_REQUIRES bt log)
