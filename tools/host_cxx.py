"""Shared C++ dialect for the host test harnesses in tools/test_*.py.

Firmware pins gnu++23 through firmware/cmake/aq_cxx_standard.cmake. Host tests
compile the same sources as strict C++23 with the host clang, so a change of
dialect happens here once instead of in every script.
"""

CXX_STD = "-std=c++23"
