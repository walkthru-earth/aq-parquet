# Board-neutral GNSS. os_gnss.cpp is pure and host tested. The reader task
# borrows a UART and pins from the board's ReaderConfig.
idf_component_register(
    SRCS "src/os_gnss.cpp" "src/os_gnss_reader.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common runtime location
    PRIV_REQUIRES esp_driver_uart esp_timer)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
