idf_component_register(SRCS "../main.cpp" INCLUDE_DIRS ".." REQUIRES connectivity)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
