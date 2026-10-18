idf_component_register(
    SRCS "src/parquet_writer.cpp" "src/parquet_reader.cpp" "src/lz4_codec.cpp"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_timer)
aq_pin_cxx_standard(${COMPONENT_LIB})
# lz4_codec.cpp includes the vendored lz4.c, whose assert() expands __FILE__.
# This component sits outside each board project, so the SDK's
# CONFIG_COMPILER_HIDE_PATHS_MACROS map does not cover it and the image would
# embed the absolute checkout path. Map it to a repo-relative path, for macros
# and debug info alike, so images built from different checkouts match and an
# ELF backtrace still resolves from the repo root.
target_compile_options(${COMPONENT_LIB} PRIVATE "-ffile-prefix-map=${COMPONENT_DIR}=firmware/common")
