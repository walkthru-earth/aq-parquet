idf_component_register(SRCS "../main.cpp" INCLUDE_DIRS ".." REQUIRES runtime esp_psram spi_flash esp_hw_support heap)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
