idf_component_register(SRCS "../main.cpp" INCLUDE_DIRS ".." REQUIRES runtime esp_psram spi_flash esp_hw_support heap)
aq_pin_cxx_standard(${COMPONENT_LIB})
