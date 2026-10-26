# Repo-relative source paths for every repo-owned ESP-IDF component.
#
# __FILE__ in assert(), ESP_ERROR_CHECK and our own log lines expands to the
# absolute path of the source. The SDK's CONFIG_COMPILER_HIDE_PATHS_MACROS
# maps only the board project directory, and most of our components live in
# firmware/common outside it, so without this map the image embeds the
# checkout path and two checkouts of one commit build different images.
#
# -ffile-prefix-map covers macros and debug info alike. A path under the repo
# becomes relative to the repo root, such as ./firmware/common/vendor/lz4/lz4.c.
# Function and line still resolve anywhere. The DWARF file name and its
# compilation directory are both relative now, so run addr2line, gdb and the
# core dump tools from the repo root to get a usable source path. addr2line
# then prints the relative path twice, which still points at the right file.
# The flag changes no code, only the path strings the compiler writes.
#
# Every board and fixture project includes this file before project.cmake, and
# every component we own calls os_map_source_paths() right after
# os_pin_cxx_standard(). Third-party components keep their macro paths.
#
# The ELF still carried checkout paths in the debug info of third-party code,
# the managed components under the board project and the NimBLE copy in the
# build directory. The app image embeds the ELF SHA-256, so those paths alone
# changed 65 bytes of the image. Each board project therefore calls
# os_map_debug_paths() after project.cmake, which maps the repo root in debug
# info only, for every component. It changes no code and no string the image
# holds.
#
# tools/check_idf_target.py fails a build when a repo C or C++ file misses the
# map, and tools/check_repro_paths.sh builds one board from two checkout paths
# and compares the images.
include_guard(GLOBAL)

get_filename_component(OS_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(OS_SOURCE_ROOT_REAL "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)

function(os_map_source_paths target)
    # GCC matches the old prefix only at a directory boundary, so the root
    # maps to ".", as the SDK maps the project directory. A checkout reached
    # through a symlink gets both spellings.
    set(options "-ffile-prefix-map=${OS_SOURCE_ROOT}=.")
    if(NOT OS_SOURCE_ROOT_REAL STREQUAL OS_SOURCE_ROOT)
        list(APPEND options "-ffile-prefix-map=${OS_SOURCE_ROOT_REAL}=.")
    endif()
    target_compile_options(${target} PRIVATE ${options})
endfunction()

function(os_map_debug_paths)
    idf_build_set_property(COMPILE_OPTIONS "-fdebug-prefix-map=${OS_SOURCE_ROOT}=." APPEND)
    if(NOT OS_SOURCE_ROOT_REAL STREQUAL OS_SOURCE_ROOT)
        idf_build_set_property(COMPILE_OPTIONS "-fdebug-prefix-map=${OS_SOURCE_ROOT_REAL}=." APPEND)
    endif()
endfunction()
