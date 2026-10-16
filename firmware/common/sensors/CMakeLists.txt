idf_component_register(
    SRCS "src/pms_frame.cpp"
    INCLUDE_DIRS "src")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
