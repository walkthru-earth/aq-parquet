idf_component_register(
    SRCS "../../logger/main.cpp" "../../board_io.cpp"
    INCLUDE_DIRS "../../logger"
    REQUIRES common sensors runtime logger power fatfs sdmmc esp_driver_sdmmc esp_driver_uart esp_driver_i2c esp_psram spi_flash led_strip esp_timer nvs_flash espcoredump)
aq_pin_cxx_standard(${COMPONENT_LIB})
aq_map_source_paths(${COMPONENT_LIB})
