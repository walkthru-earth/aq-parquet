#pragma once
#include "Print.h"

class FakeSerial : public Print {
public:
  std::size_t write(std::uint8_t) override { return 1; }
  std::size_t write(const std::uint8_t *, std::size_t size) override {
    return size;
  }
};
inline FakeSerial Serial;
