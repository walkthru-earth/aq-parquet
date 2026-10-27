idf_component_register(
    SRCS "src/os_upload_sigv4.cpp" "src/os_upload_sts.cpp" "src/os_upload_policy.cpp" "src/os_upload_ledger.cpp" "src/os_upload_session.cpp" "src/os_upload_device.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common
    PRIV_REQUIRES esp_http_client mbedtls)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
