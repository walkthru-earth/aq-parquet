idf_component_register(
    SRCS "src/parquet_writer.cpp" "src/parquet_reader.cpp" "src/lz4_codec.cpp"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_timer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
