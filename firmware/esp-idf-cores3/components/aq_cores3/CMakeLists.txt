idf_component_register(
    SRCS "../../bringup/main.cpp" "../../bringup/telemetry_logger.cpp"
    INCLUDE_DIRS "../../bringup"
    REQUIRES common runtime logger m5unified fatfs sdmmc esp_driver_sdspi esp_driver_uart esp_psram spi_flash nvs_flash esp_timer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
