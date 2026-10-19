idf_component_register(
    SRCS "src/pms_frame.cpp"
    INCLUDE_DIRS "src")
aq_pin_cxx_standard(${COMPONENT_LIB})
aq_map_source_paths(${COMPONENT_LIB})
