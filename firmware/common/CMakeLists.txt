idf_component_register(
    SRCS "src/parquet_writer.cpp" "src/parquet_reader.cpp" "src/lz4_codec.cpp"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_timer)
aq_pin_cxx_standard(${COMPONENT_LIB})
