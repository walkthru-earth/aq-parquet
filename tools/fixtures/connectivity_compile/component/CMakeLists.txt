idf_component_register(SRCS "../main.cpp" INCLUDE_DIRS ".." REQUIRES connectivity)
aq_pin_cxx_standard(${COMPONENT_LIB})
aq_map_source_paths(${COMPONENT_LIB})
