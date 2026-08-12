set(BLUFI_SRCS "")
if(CONFIG_BLUFI_PROVISIONING_ENABLED)
    list(APPEND BLUFI_SRCS "blufi_provisioning.c" "blufi_security.c")
endif()

idf_component_register(SRCS "${BLUFI_SRCS}"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager ble_host
    PRIV_REQUIRES bt nvs_flash mbedtls esp_hw_support esp_event esp_wifi log freertos)
