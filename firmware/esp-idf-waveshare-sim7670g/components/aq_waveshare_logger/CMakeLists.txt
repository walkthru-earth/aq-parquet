idf_component_register(
    SRCS "../../logger/main.cpp" "../../board_io.cpp"
    INCLUDE_DIRS "../../logger"
    REQUIRES common runtime logger fatfs sdmmc esp_driver_sdmmc esp_driver_uart esp_driver_i2c esp_psram spi_flash led_strip esp_timer nvs_flash)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
