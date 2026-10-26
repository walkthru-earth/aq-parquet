idf_component_register(
    SRCS "../../diagnostic/main.cpp" "../../board_io.cpp"
    INCLUDE_DIRS "../../diagnostic"
    REQUIRES common sensors runtime fatfs sdmmc esp_driver_sdmmc esp_driver_uart spi_flash esp_driver_i2c led_strip esp_timer espcoredump)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
