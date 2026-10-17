# Shared C++ dialect for every repo-owned ESP-IDF component.
#
# ESP-IDF 6.1 adds -std=gnu++26 for chip targets, and a later SDK may move it
# again. Pin the dialect here so an SDK upgrade never changes it silently.
# Every board and fixture project includes this file before project.cmake,
# and every component we own calls aq_pin_cxx_standard() right after
# idf_component_register(). Third-party components keep the SDK default.
#
# The generator expression limits the flag to C++ sources, so components that
# also compile C (the vendored H3 library in location) keep the C dialect.
include_guard(GLOBAL)

set(AQ_CXX_STANDARD_FLAG "-std=gnu++23")

function(aq_pin_cxx_standard target)
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${AQ_CXX_STANDARD_FLAG}>")
endfunction()
