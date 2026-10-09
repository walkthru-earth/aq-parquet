#include "network_time.h"
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <lwip/dns.h>

#include <cassert>
#include <cstdio>
#include <cstring>

namespace ntp = aq::network_time;
namespace {
void reset(const ntp::Policy &policy) {
  ntp::Policy disabled;
  disabled.enabled = false;
  assert(ntp::configure(disabled));
  assert(ntp::configure(policy));
  ntp::prepare_network();
  ntp::set_network_available(true);
  fake_ntp_candidates.clear();
  fake_dns_requests.clear();
}
void advance_response() {
  fake_now_us += 15000000;
  ntp::poll(); // expire selected candidate
  ntp::poll(); // select next candidate
}
void sync(std::int64_t seconds = 1800000000, int micros = 456789) {
  assert(fake_time_callback);
  timeval value{seconds, micros};
  fake_time_callback(&value);
}
} // namespace

int main() {
  ntp::Anchor anchor;
  assert(!ntp::take_anchor(anchor) && !ntp::status().has_anchor);
  ntp::Policy policy;
  policy.interval_s = 60;
  std::strcpy(policy.servers[0], "192.168.1.10");
  std::strcpy(policy.servers[1], "local.example.test");
  fake_dns_answers["local.example.test"] = "192.168.1.11";
  reset(policy);
  assert(fake_dhcp_ntp);
  fake_dhcp_ntp_offer("192.168.1.1");
  ntp::ip_changed();
  ntp::poll();
  assert(fake_ntp_candidates == std::vector<std::string>{"192.168.1.1"});
  assert(fake_dns_requests.empty() && !ntp::status().has_anchor);
  // All slots address the same local endpoint even if native SDK kept an old
  // server index. DHCP-derived addresses remain cached throughout failover.
  assert(std::strcmp(fake_ntp_names[0], fake_ntp_names[2]) == 0);
  advance_response();
  assert(fake_ntp_candidates.back() == "192.168.1.10");
  assert(fake_dns_requests.empty());
  advance_response();
  assert(fake_dns_requests == std::vector<std::string>{"local.example.test"});
  ntp::poll();
  assert(fake_ntp_candidates.back() == "192.168.1.11");
  advance_response();
  assert(fake_dns_requests.back() == "time.cloudflare.com");
  ntp::poll();
  assert(fake_ntp_candidates.back() == "203.0.113.1");
  assert(!fake_dhcp_ntp);
  fake_dhcp_ntp_offer("192.168.9.1"); // active request slots stay frozen
  assert(std::strcmp(fake_ntp_names[0], "203.0.113.1") == 0);
  assert(!ntp::take_anchor(anchor)); // fallback availability is not evidence
  const auto callback_mono = fake_now_us;
  sync();
  fake_now_us += 1234567; // delayed logger must retain callback's pairing
  assert(ntp::take_anchor(anchor));
  assert(anchor.seconds == 1800000000 && anchor.subsecond_us == 456789 &&
         anchor.monotonic_us == callback_mono);
  assert(!ntp::take_anchor(anchor));
  auto state = ntp::status();
  assert(state.has_anchor && state.source == 3 && state.public_active &&
         state.age_s == 1 &&
         std::strcmp(state.server, "time.cloudflare.com") == 0);
  const auto count = state.sync_count;
  sync(1); // unsupported UTC cannot pollute provenance or pending anchor
  sync(1800000000, 1000000);
  assert(ntp::status().sync_count == count && !ntp::take_anchor(anchor));
  ntp::poll(); // stop successful request, future refresh starts with local
  assert(!ntp::status().running && !fake_time_callback);
  assert(fake_dhcp_ntp); // renewals accepted again between requests
  const auto starts = fake_ntp_starts;
  fake_now_us = callback_mono + 60000000 - 1;
  ntp::poll();
  assert(fake_ntp_starts == starts);
  ++fake_now_us;
  ntp::poll();
  assert(fake_ntp_candidates.back() == "192.168.1.1");
  // Disconnect retains actual UTC evidence, with increasing age, but drops
  // the native callback and never submits a synthetic network anchor.
  ntp::set_network_available(false);
  fake_now_us += 400000000;
  state = ntp::status();
  assert(std::strcmp(state.state, "offline") == 0 && state.has_anchor &&
         state.stale && state.age_s > 360 && state.source == 3);
  assert(!ntp::take_anchor(anchor));

  // Router success must never contact public DNS or pause for a fallback.
  reset(policy);
  fake_dhcp_ntp_offer("192.168.2.1");
  ntp::ip_changed();
  ntp::poll();
  sync();
  assert(ntp::take_anchor(anchor));
  ntp::poll();
  assert(fake_dns_requests.empty() && !ntp::status().public_active &&
         std::strcmp(ntp::status().server, "192.168.2.1") == 0);

  // Explicit policy disabling public fallback is enforced through failure
  // and retry. No DHCP offer means no invented gateway time server.
  policy.public_fallback = false;
  reset(policy);
  ntp::ip_changed();
  ntp::poll();
  assert(fake_ntp_candidates.front() == "192.168.1.10");
  advance_response();
  ntp::poll();
  advance_response();
  assert(std::strcmp(ntp::status().state, "retry") == 0);
  for (const auto &name : fake_dns_requests)
    assert(name != "time.cloudflare.com");
  const auto retries = fake_ntp_starts;
  fake_now_us += 30000000 - 1;
  ntp::poll();
  assert(fake_ntp_starts == retries);
  ++fake_now_us;
  ntp::poll();
  assert(fake_ntp_starts == retries + 1);

  // Native init errors retry no more often than once per 30 seconds.
  policy.dhcp = false;
  policy.servers[1][0] = '\0';
  reset(policy);
  assert(!fake_dhcp_ntp);
  fake_ntp_init_result = ESP_FAIL;
  ntp::poll();
  const auto failed_starts = fake_ntp_starts;
  ntp::poll();
  assert(fake_ntp_starts == failed_starts && ntp::status().error == ESP_FAIL);
  fake_now_us += 30000000 - 1;
  ntp::poll();
  assert(fake_ntp_starts == failed_starts);
  fake_ntp_init_result = ESP_OK;
  ++fake_now_us;
  ntp::poll();
  assert(fake_ntp_starts == failed_starts + 1 && ntp::status().running);

  // Invalid input stops native service without erasing a previous anchor.
  std::strcpy(policy.servers[0], "https://bad.example");
  assert(!ntp::configure(policy));
  assert(!fake_time_callback && ntp::status().has_anchor &&
         std::strcmp(ntp::status().state, "invalid") == 0);
  ntp::prepare_network();
  assert(std::strcmp(ntp::status().state, "invalid") == 0);
  ntp::set_network_available(true);
  policy.servers[0][0] = '\0';
  assert(ntp::configure(policy));
  ntp::poll();
  assert(std::strcmp(ntp::status().state, "unconfigured") == 0);
  policy.interval_s = 59;
  assert(!ntp::configure(policy));
  policy.interval_s = 86401;
  assert(!ntp::configure(policy));
  policy.interval_s = 60;
  std::memset(policy.servers[0], 'x', sizeof(policy.servers[0]));
  assert(!ntp::configure(policy));
  std::memset(policy.servers[0], 0, sizeof(policy.servers[0]));

  // Reconfiguration during DNS cannot let an old callback launch SNTP on an
  // old endpoint, even when it completes before the new query.
  fake_dns_deferred = true;
  std::strcpy(policy.servers[0], "old.example.test");
  reset(policy);
  ntp::poll();
  assert(fake_dns_queries.size() == 1);
  std::strcpy(policy.servers[0], "new.example.test");
  assert(ntp::configure(policy));
  ntp::poll();
  assert(fake_dns_queries.size() == 2);
  const auto before_dns = fake_ntp_starts;
  fake_dns_complete(0, "198.51.100.9");
  ntp::poll();
  assert(fake_ntp_starts == before_dns);
  fake_dns_complete(0, "198.51.100.10");
  ntp::poll();
  assert(fake_ntp_starts == before_dns + 1 &&
         fake_ntp_candidates.back() == "198.51.100.10");
  sync();
  assert(ntp::take_anchor(anchor));
  assert(std::strcmp(ntp::status().server, "new.example.test") == 0);

  // DNS timeout also ignores its late completion; there is no native SDK DNS
  // callback holding a destroyed SNTP PCB. Query memory remains bounded.
  std::strcpy(policy.servers[0], "timeout.example.test");
  policy.public_fallback = true;
  reset(policy);
  ntp::poll();
  fake_now_us += 30000000;
  ntp::poll();
  ntp::poll();
  assert(fake_dns_queries.size() == 2);
  const auto timeout_starts = fake_ntp_starts;
  fake_dns_complete(0, "198.51.100.11");
  ntp::poll();
  assert(fake_ntp_starts == timeout_starts);
  fake_dns_complete(0, "203.0.113.1");
  ntp::poll();
  assert(fake_ntp_candidates.back() == "203.0.113.1");
  ntp::set_network_available(false);
  const auto saved_callback = fake_time_callback;
  assert(saved_callback == nullptr);

  // Four stale outstanding DNS requests exhaust the pool safely; completion
  // releases storage and bounded retry eventually proceeds.
  policy.public_fallback = false;
  for (unsigned i = 0; i < 5; ++i) {
    std::snprintf(policy.servers[0], sizeof(policy.servers[0]),
                  "pending%u.example.test", i);
    assert(ntp::configure(policy));
    ntp::set_network_available(true);
    ntp::poll();
  }
  assert(fake_dns_queries.size() == 4 && ntp::status().error == ESP_ERR_NO_MEM);
  while (!fake_dns_queries.empty())
    fake_dns_complete(0, nullptr);
  fake_now_us += 30000000;
  ntp::poll();
  assert(fake_dns_queries.size() == 1);
  ntp::set_network_available(false);
  fake_dns_complete(0, "198.51.100.12");
  ntp::poll();
  assert(!fake_time_callback && !ntp::take_anchor(anchor));
  std::puts("Native network time: router/local/public priority, callbacks, "
            "age, async DNS generations and bounded retries passed");
}
