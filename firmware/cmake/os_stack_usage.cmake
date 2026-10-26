# Static stack usage and call graph output for every object in the image.
#
# GCC writes `<object>.su` (one frame size per function) and `<object>.ci`
# (the per object call graph with those frame sizes) next to each object in
# the build directory. tools/stack_budget.py joins the .ci files into one
# graph and reports the static worst case of each tracked task (issue #37).
#
# Neither flag changes code generation, so the image bytes stay the same. The
# files never reach flash and are rebuilt with their objects. Every board
# project calls os_enable_stack_usage() after including project.cmake and
# before project(), so the flags reach SDK components such as NimBLE too and
# the graph can follow our callbacks back to the task that runs them.
include_guard(GLOBAL)

function(os_enable_stack_usage)
    idf_build_set_property(COMPILE_OPTIONS "-fstack-usage" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-fcallgraph-info=su" APPEND)
endfunction()
