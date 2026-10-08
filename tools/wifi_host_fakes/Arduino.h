#pragma once
#include <string>
class String {
public:
  String(const char *value = "") : value_(value) {}
  String(unsigned value) : value_(std::to_string(value)) {}
  const char *c_str() const { return value_.c_str(); }
  std::size_t length() const { return value_.size(); }
  bool operator==(const String &other) const { return value_ == other.value_; }

private:
  std::string value_;
};
