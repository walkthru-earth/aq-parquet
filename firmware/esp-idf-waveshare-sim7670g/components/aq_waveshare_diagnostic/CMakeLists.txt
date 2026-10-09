idf_component_register(
    SRCS "../../diagnostic/main.cpp" "../../board_io.cpp"
    INCLUDE_DIRS "../../diagnostic"
    REQUIRES common runtime fatfs sdmmc esp_driver_sdmmc esp_driver_uart spi_flash esp_driver_i2c led_strip esp_timer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
