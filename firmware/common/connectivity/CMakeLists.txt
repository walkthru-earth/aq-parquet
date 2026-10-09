idf_component_register(
    SRCS "src/archive_sync.cpp" "src/ble_sync.cpp" "src/control_sync.cpp"
         "src/sync_service.cpp" "src/wifi_link.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common runtime
    PRIV_REQUIRES bt esp_timer esp_wifi esp_netif esp_event lwip mdns)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
