# Board-neutral GNSS reader task. A board with a receiver REQUIRES it and
# fills ReaderConfig with its UART, pins and power callback.
idf_component_register(
    SRCS "src/os_gnss_reader.cpp"
    INCLUDE_DIRS "src"
    REQUIRES gnss
    PRIV_REQUIRES common runtime esp_driver_uart esp_timer)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
