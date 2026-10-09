idf_component_register(
    SRCS "src/device_config.cpp" "src/debug_log.cpp" "src/aq_console.cpp"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES location lwip nvs_flash esp_hw_support esp_driver_uart esp_driver_usb_serial_jtag vfs)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
