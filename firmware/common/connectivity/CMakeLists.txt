idf_component_register(
    SRCS "src/archive_sync.cpp" "src/ble_sync.cpp" "src/control_sync.cpp"
         "src/sync_service.cpp" "src/wifi_link.cpp" "src/network_time.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common runtime
    PRIV_REQUIRES bt esp_timer esp_wifi esp_netif esp_event lwip mdns)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# Match both endpoint and request originate timestamp before accepting SNTP.
# ESP-IDF 6.1's lwIP default is zero; no SDK/vendor source edit is required.
idf_component_get_property(aq_lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${aq_lwip_lib} PRIVATE SNTP_CHECK_RESPONSE=2)
