idf_component_register(
    SRCS "src/pms_frame.cpp"
    INCLUDE_DIRS "src" "modules")
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
