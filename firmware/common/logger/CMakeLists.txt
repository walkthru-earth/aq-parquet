idf_component_register(
    SRCS "src/aq_logger.cpp" "src/aq_logger_status.cpp" "src/aq_logger_provision.cpp" "src/aq_logger_live.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common runtime connectivity location power
    PRIV_REQUIRES nvs_flash esp_timer esp_hw_support heap)
aq_pin_cxx_standard(${COMPONENT_LIB})
