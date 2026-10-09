idf_component_register(
    SRCS "src/aq_logger.cpp" "src/aq_logger_status.cpp" "src/aq_logger_provision.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common runtime connectivity location
    PRIV_REQUIRES nvs_flash esp_timer esp_hw_support heap)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
