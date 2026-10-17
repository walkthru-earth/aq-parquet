"""Shared C++ dialect for the host test harnesses in tools/test_*.py.

Host tests compile the firmware sources as strict C++23 with the host clang.
The same standard is named in three other places, the firmware CMake helper
firmware/cmake/aq_cxx_standard.cmake, the pms-frame-test task in pixi.toml and
the cppcheck options in tools/lint.sh. Change all four together.
`python tools/check_idf_target.py --cxx-dialect`, run by tools/lint.sh and by
every board build, fails when they disagree.
"""

CXX_STD = "-std=c++23"
