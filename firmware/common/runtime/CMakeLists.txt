idf_component_register(
    SRCS "src/device_config.cpp" "src/debug_log.cpp" "src/log_persist.cpp" "src/aq_console.cpp" "src/power.cpp" "src/coredump_report.cpp"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES espcoredump esp_partition esp_pm hal location lwip nvs_flash esp_hw_support esp_driver_uart esp_driver_usb_serial_jtag esp_psram vfs)
aq_pin_cxx_standard(${COMPONENT_LIB})
