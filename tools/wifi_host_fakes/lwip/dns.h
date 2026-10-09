#pragma once
#include "ip_addr.h"
#include <map>
#include <string>
#include <vector>
using err_t = int;
constexpr err_t ERR_OK = 0, ERR_INPROGRESS = -5, ERR_ARG = -16;
using dns_found_callback = void (*)(const char *, const ip_addr_t *, void *);
struct FakeDnsQuery {
  std::string name;
  dns_found_callback callback;
  void *argument;
};
inline bool fake_dns_deferred = false;
inline std::map<std::string, std::string> fake_dns_answers = {
    {"time.cloudflare.com", "203.0.113.1"}};
inline std::vector<FakeDnsQuery> fake_dns_queries;
inline std::vector<std::string> fake_dns_requests;
inline err_t dns_gethostbyname(const char *name, ip_addr_t *out,
                               dns_found_callback callback, void *argument) {
  fake_dns_requests.emplace_back(name);
  if (fake_dns_deferred) {
    fake_dns_queries.push_back({name, callback, argument});
    return ERR_INPROGRESS;
  }
  const auto answer = fake_dns_answers.find(name);
  return answer != fake_dns_answers.end() &&
                 ipaddr_aton(answer->second.c_str(), out)
             ? ERR_OK
             : ERR_ARG;
}
inline void fake_dns_complete(unsigned index, const char *literal) {
  const auto query = fake_dns_queries.at(index);
  fake_dns_queries.erase(fake_dns_queries.begin() + index);
  ip_addr_t address{};
  query.callback(query.name.c_str(),
                 literal && ipaddr_aton(literal, &address) ? &address : nullptr,
                 query.argument);
}
