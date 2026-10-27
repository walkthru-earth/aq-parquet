# Board-neutral GNSS rules. Pure code, host tested, linked by the logger on
# every board. The UART reader task lives in ../gnss_reader, so a board
# without a receiver never compiles it.
idf_component_register(
    SRCS "src/os_gnss.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common runtime location)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
