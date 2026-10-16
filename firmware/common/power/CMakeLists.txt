idf_component_register(
    SRCS "src/aq_battery_health.cpp"
    INCLUDE_DIRS "src")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
