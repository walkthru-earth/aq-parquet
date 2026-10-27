// Host tests for the device uploader (#81), firmware/common/upload.
//
// The SigV4 vectors below come from the AWS signing test suite in
// awslabs/aws-c-auth (tests/aws-signing-test-suite/v4), Apache-2.0,
// "AWS C Auth, Copyright Amazon.com, Inc. or its affiliates". They use the
// public AKIDEXAMPLE example credentials, which sign nothing real.
//
// Never put a real or realistic API key, PSK, PIN or token in this file.

#include "../firmware/common/upload/src/os_upload_ledger.h"
#include "../firmware/common/upload/src/os_upload_policy.h"
#include "../firmware/common/upload/src/os_upload_session.h"
#include "../firmware/common/upload/src/os_upload_sigv4.h"
#include "../firmware/common/upload/src/os_upload_sts.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace opensensor::upload;

namespace {

int checks = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__,     \
                   #cond);                                                     \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

// FIPS 180-4 SHA-256, a plain reference for the host. The device uses PSA.
class RefSha256 : public Sha256 {
public:
  void begin() override {
    state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    length_ = 0;
    used_ = 0;
  }
  void update(std::span<const std::uint8_t> data) override {
    for (const std::uint8_t b : data) {
      block_[used_++] = b;
      if (used_ == 64) {
        compress();
        used_ = 0;
      }
    }
    length_ += data.size();
  }
  using Sha256::update;
  bool finish(Digest &out) override {
    const std::uint64_t bits = length_ * 8;
    block_[used_++] = 0x80;
    if (used_ > 56) {
      while (used_ < 64)
        block_[used_++] = 0;
      compress();
      used_ = 0;
    }
    while (used_ < 56)
      block_[used_++] = 0;
    for (int i = 7; i >= 0; --i)
      block_[used_++] = static_cast<std::uint8_t>(bits >> (8 * i));
    compress();
    for (std::size_t i = 0; i < 8; ++i)
      for (std::size_t j = 0; j < 4; ++j)
        out[4 * i + j] = static_cast<std::uint8_t>(state_[i] >> (24 - 8 * j));
    return true;
  }

private:
  static std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
  }
  void compress() {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
      w[i] = (std::uint32_t(block_[4 * i]) << 24) |
             (std::uint32_t(block_[4 * i + 1]) << 16) |
             (std::uint32_t(block_[4 * i + 2]) << 8) | block_[4 * i + 3];
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 =
          rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 =
          rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                               ((e & f) ^ (~e & g)) + k[i] + w[i];
      const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                               ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    const std::array<std::uint32_t, 8> add = {a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < 8; ++i)
      state_[i] += add[i];
  }

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> block_{};
  std::size_t used_ = 0;
  std::uint64_t length_ = 0;
};

// Records the bytes of the first digest, the canonical request.
class TeeSha256 : public RefSha256 {
public:
  void begin() override {
    ++digests_;
    RefSha256::begin();
  }
  void update(std::span<const std::uint8_t> data) override {
    if (digests_ == 1)
      first_.append(reinterpret_cast<const char *>(data.data()), data.size());
    RefSha256::update(data);
  }
  using Sha256::update;
  std::string first_;

private:
  int digests_ = 0;
};

std::span<const std::uint8_t> bytes(std::string_view s) {
  return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}

std::string hex(const Digest &d) {
  HexDigest h{};
  to_hex(d, h);
  return std::string(h.data(), h.size());
}

std::string sv(const HexDigest &h) { return std::string(h.data(), h.size()); }

void test_sha256_and_hmac() {
  RefSha256 hash;
  Digest d{};
  CHECK(sha256(hash, bytes("abc"), d));
  CHECK(hex(d) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256(hash, bytes(""), d));
  CHECK(hex(d) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256(
      hash, bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      d));
  CHECK(hex(d) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // RFC 4231 test case 1, 2 and 6 (a key longer than one block).
  std::array<std::uint8_t, 20> k1{};
  k1.fill(0x0b);
  CHECK(hmac_sha256(hash, k1, bytes("Hi There"), d));
  CHECK(hex(d) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  CHECK(hmac_sha256(hash, bytes("Jefe"), bytes("what do ya want for nothing?"),
                    d));
  CHECK(hex(d) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  std::array<std::uint8_t, 131> k6{};
  k6.fill(0xaa);
  CHECK(hmac_sha256(
      hash, k6, bytes("Test Using Larger Than Block-Size Key - Hash Key First"),
      d));
  CHECK(hex(d) ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

constexpr std::string_view kAkid = "AKIDEXAMPLE";
constexpr std::string_view kSecret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
constexpr std::string_view kDate = "20150830T123600Z";
constexpr std::string_view kHost = "example.amazonaws.com";
constexpr std::string_view kEmpty =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

SignRequest vanilla(std::string_view method, std::span<const Header> headers,
                    std::string_view payload = kEmpty) {
  return {.method = method,
          .canonical_uri = "/",
          .canonical_query = "",
          .headers = headers,
          .payload_sha256_hex = payload,
          .amz_date = kDate,
          .region = "us-east-1",
          .service = "service",
          .access_key_id = kAkid,
          .secret_access_key = kSecret};
}

void test_sigv4_vectors() {
  const std::array basic = {Header{.name = "host", .value = kHost},
                            Header{.name = "x-amz-date", .value = kDate}};
  {
    TeeSha256 hash;
    const auto sig = signature(hash, vanilla("GET", basic));
    CHECK(sig.has_value());
    CHECK(sv(*sig) ==
          "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
    CHECK(hash.first_ == "GET\n/\n\nhost:example.amazonaws.com\n"
                         "x-amz-date:20150830T123600Z\n\nhost;x-amz-date\n" +
                             std::string(kEmpty));
  }
  {
    RefSha256 hash;
    const auto sig = signature(hash, vanilla("POST", basic));
    CHECK(
        sig &&
        sv(*sig) ==
            "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b");
    std::array<char, 256> auth{};
    const auto size = authorization(hash, vanilla("POST", basic), auth);
    CHECK(size.has_value());
    CHECK(std::string_view(auth.data(), *size) ==
          "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/"
          "aws4_request, SignedHeaders=host;x-amz-date, "
          "Signature="
          "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b");
    CHECK(auth[*size] == '\0');
    std::array<char, 40> small{};
    CHECK(authorization(hash, vanilla("POST", basic), small).error() ==
          SignError::Overflow);
  }
  {
    // The STS shape, a form body with its hash signed in a header.
    constexpr std::string_view body_hash =
        "9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e";
    RefSha256 hash;
    Digest d{};
    CHECK(sha256(hash, bytes("Param1=value1"), d) && hex(d) == body_hash);
    const std::array form = {
        Header{.name = "content-length", .value = "13"},
        Header{.name = "content-type",
               .value = "application/x-www-form-urlencoded"},
        Header{.name = "host", .value = kHost},
        Header{.name = "x-amz-content-sha256", .value = body_hash},
        Header{.name = "x-amz-date", .value = kDate}};
    TeeSha256 tee;
    const auto sig = signature(tee, vanilla("POST", form, body_hash));
    CHECK(
        sig &&
        sv(*sig) ==
            "d3875051da38690788ef43de4db0d8f280229d82040bfac253562e56c3f20e0b");
    CHECK(tee.first_.find(
              "\ncontent-length;content-type;host;x-amz-content-sha256;"
              "x-amz-date\n") != std::string::npos);
  }
  {
    const std::array token = {
        Header{.name = "host", .value = kHost},
        Header{.name = "x-amz-date", .value = kDate},
        Header{.name = "x-amz-security-token",
               .value = "6e86291e8372ff2a2260956d9b8aae1d763fbf315fa00fa31553b"
                        "73ebf194267"}};
    RefSha256 hash;
    const auto sig = signature(hash, vanilla("GET", token));
    CHECK(
        sig &&
        sv(*sig) ==
            "07ec1639c89043aa0e3e2de82b96708f198cceab042d4a97044c66dd9f74e7f8");
  }
}

void test_sigv4_rejects() {
  RefSha256 hash;
  const auto fails = [&](std::span<const Header> h, SignError e,
                         std::string_view payload = kEmpty) {
    const auto sig = signature(hash, vanilla("GET", h, payload));
    return !sig && sig.error() == e;
  };
  const std::array unsorted = {Header{.name = "x-amz-date", .value = kDate},
                               Header{.name = "host", .value = kHost}};
  CHECK(fails(unsorted, SignError::BadHeaders));
  const std::array duplicate = {Header{.name = "host", .value = kHost},
                                Header{.name = "host", .value = kHost},
                                Header{.name = "x-amz-date", .value = kDate}};
  CHECK(fails(duplicate, SignError::BadHeaders));
  const std::array no_host = {Header{.name = "x-amz-date", .value = kDate}};
  CHECK(fails(no_host, SignError::BadHeaders));
  const std::array no_date = {Header{.name = "host", .value = kHost}};
  CHECK(fails(no_date, SignError::BadHeaders));
  const std::array other_date = {
      Header{.name = "host", .value = kHost},
      Header{.name = "x-amz-date", .value = "20150830T123601Z"}};
  CHECK(fails(other_date, SignError::BadDate));
  const std::array upper = {Header{.name = "Host", .value = kHost},
                            Header{.name = "x-amz-date", .value = kDate}};
  CHECK(fails(upper, SignError::BadHeaders));
  const std::array newline = {
      Header{.name = "host", .value = "example.amazonaws.com\nx-evil:1"},
      Header{.name = "x-amz-date", .value = kDate}};
  CHECK(fails(newline, SignError::BadHeaders));
  const std::array spaces = {Header{.name = "a", .value = "x  y"},
                             Header{.name = "host", .value = kHost},
                             Header{.name = "x-amz-date", .value = kDate}};
  CHECK(fails(spaces, SignError::BadHeaders));
  const std::array basic = {Header{.name = "host", .value = kHost},
                            Header{.name = "x-amz-date", .value = kDate}};
  CHECK(fails(basic, SignError::BadPayloadHash, "E3B0"));
  CHECK(fails(
      basic, SignError::BadPayloadHash,
      "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"));
  SignRequest r = vanilla("GET", basic);
  r.region = "us east";
  CHECK(signature(hash, r).error() == SignError::BadScope);
  r = vanilla("GET", basic);
  r.access_key_id = "AKID/EXAMPLE";
  CHECK(signature(hash, r).error() == SignError::BadCredentials);
  r = vanilla("GET", basic);
  r.secret_access_key = "";
  CHECK(signature(hash, r).error() == SignError::BadCredentials);
  r = vanilla("GET", basic);
  r.canonical_uri = "/a b";
  CHECK(signature(hash, r).error() == SignError::BadHeaders);
  r = vanilla("get", basic);
  CHECK(signature(hash, r).error() == SignError::BadHeaders);
}

void test_dates_and_encoding() {
  // 2026-10-27T09:05:07Z
  const auto d = format_amz_date(1793091907);
  CHECK(d && std::string_view(d->data(), d->size()) == "20261027T090507Z");
  const auto leap = format_amz_date(1709164800); // 2024-02-29T00:00:00Z
  CHECK(leap &&
        std::string_view(leap->data(), leap->size()) == "20240229T000000Z");
  CHECK(!format_amz_date(1440938160)); // 2015, before supported clocks
  CHECK(!format_amz_date(4102444800)); // 2100
  CHECK(!format_amz_date(-1));

  std::array<char, 64> out{};
  auto n = encode_path("/walkthru-earth/a b=c/~x_y.z", out);
  CHECK(n &&
        std::string_view(out.data(), *n) == "/walkthru-earth/a%20b%3Dc/~x_y.z");
  n = encode_component("a/b:c", out);
  CHECK(n && std::string_view(out.data(), *n) == "a%2Fb%3Ac");
  std::array<char, 4> tiny{};
  CHECK(!encode_path("/abcd", tiny));
}

// Public AWS documentation example values, not credentials.
constexpr std::string_view kStsOk =
    "<AssumeRoleWithWebIdentityResponse "
    "xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">\n"
    "  <AssumeRoleWithWebIdentityResult>\n"
    "    <SubjectFromWebIdentityToken>example</SubjectFromWebIdentityToken>\n"
    "    <AssumedRoleUser>\n"
    "      <Arn>arn:aws:sts::123456789012:assumed-role/Example/app</Arn>\n"
    "      <AssumedRoleId>AROACLKWSDQRAOEXAMPLE:app</AssumedRoleId>\n"
    "    </AssumedRoleUser>\n"
    "    <Credentials>\n"
    "      <SessionToken>AQoDYXdzEE0a8ANXXXXXXXXNO1ewxE5TijQyp+IEXAMPLE"
    "</SessionToken>\n"
    "      <SecretAccessKey>wJalrXUtnFEMI/K7MDENG/bPxRfiCYzEXAMPLEKEY"
    "</SecretAccessKey>\n"
    "      <Expiration>2026-10-27T10:00:00.000Z</Expiration>\n"
    "      <AccessKeyId>ASIAIOSFODNN7EXAMPLE</AccessKeyId>\n"
    "    </Credentials>\n"
    "  </AssumeRoleWithWebIdentityResult>\n"
    "  <ResponseMetadata><RequestId>ad4156e9</RequestId></ResponseMetadata>\n"
    "</AssumeRoleWithWebIdentityResponse>\n";

constexpr std::string_view kStsDenied =
    "<ErrorResponse xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"
    "<Error><Type>Sender</Type><Code>InvalidIdentityToken</Code>"
    "<Message>refused</Message></Error><RequestId>x</RequestId>"
    "</ErrorResponse>";

std::string replace(std::string s, std::string_view from, std::string_view to) {
  const auto at = s.find(from);
  assert(at != std::string::npos);
  s.replace(at, from.size(), to);
  return s;
}

void test_sts() {
  std::array<char, kStsBodyMax> body{};
  const auto size =
      build_sts_body({.role_arn = "arn:aws:iam::example--board:role/FullAccess",
                      .session_name = "cores3",
                      .web_identity_token = "<sck-key>"},
                     body);
  CHECK(size.has_value());
  CHECK(std::string_view(body.data(), *size) ==
        "Action=AssumeRoleWithWebIdentity&DurationSeconds=3600&RoleArn=arn%"
        "3Aaws%3Aiam%3A%3Aexample--board%3Arole%2FFullAccess&RoleSessionName="
        "cores3&Version=2011-06-15&WebIdentityToken=%3Csck-key%3E");
  const StsParams ok{.role_arn = "arn:x",
                     .session_name = "cores3",
                     .web_identity_token = "<sck-key>"};
  StsParams p = ok;
  p.web_identity_token = "<sck key>";
  CHECK(build_sts_body(p, body).error() == StsBuildError::BadToken);
  p = ok;
  p.web_identity_token = "";
  CHECK(build_sts_body(p, body).error() == StsBuildError::BadToken);
  p = ok;
  p.role_arn = "role/FullAccess";
  CHECK(build_sts_body(p, body).error() == StsBuildError::BadRoleArn);
  p = ok;
  p.session_name = "a b";
  CHECK(build_sts_body(p, body).error() == StsBuildError::BadSessionName);
  p = ok;
  p.duration_s = 60;
  CHECK(build_sts_body(p, body).error() == StsBuildError::BadDuration);
  std::array<char, 40> small{};
  CHECK(build_sts_body(ok, small).error() == StsBuildError::Overflow);

  Credentials creds;
  CHECK(parse_sts_response(kStsOk, creds).has_value());
  CHECK(creds.key_id() == "ASIAIOSFODNN7EXAMPLE");
  CHECK(creds.secret() == "wJalrXUtnFEMI/K7MDENG/bPxRfiCYzEXAMPLEKEY");
  CHECK(creds.token() == "AQoDYXdzEE0a8ANXXXXXXXXNO1ewxE5TijQyp+IEXAMPLE");
  CHECK(creds.expires_utc_s == 1793095200); // 2026-10-27T10:00:00Z
  creds.wipe();
  CHECK(!creds.present() && creds.token().empty());

  // Every truncation fails and leaves no partial credentials behind. The
  // document ends in ">\n", so the last two prefixes are complete.
  for (std::size_t n = 0; n + 1 < kStsOk.size(); ++n) {
    Credentials c;
    c.access_key_id[0] = 'X';
    const auto r = parse_sts_response(kStsOk.substr(0, n), c);
    CHECK(!r.has_value());
    CHECK(!c.present());
  }
  CHECK(parse_sts_response(kStsOk.substr(0, 100), creds).error() ==
        StsParseError::Truncated);

  const std::string good(kStsOk);
  const auto parse = [&](const std::string &xml) {
    Credentials c;
    const auto r = parse_sts_response(xml, c);
    CHECK(r.has_value() || !c.present());
    return r;
  };
  CHECK(parse(replace(good, "<AccessKeyId>",
                      "<AccessKeyId>A</AccessKeyId><AccessKeyId>"))
            .error() == StsParseError::Malformed);
  CHECK(parse(replace(good, "<AccessKeyId>ASIAIOSFODNN7EXAMPLE</AccessKeyId>",
                      ""))
            .error() == StsParseError::MissingField);
  CHECK(parse(replace(good, "ASIAIOSFODNN7EXAMPLE", "ASIA&amp;X")).error() ==
        StsParseError::Malformed);
  CHECK(parse(replace(good, "ASIAIOSFODNN7EXAMPLE", "ASIA<b>X</b>")).error() ==
        StsParseError::Malformed);
  CHECK(parse(replace(good, "ASIAIOSFODNN7EXAMPLE", "ASIA/X")).error() ==
        StsParseError::Malformed);
  CHECK(parse(replace(good, "ASIAIOSFODNN7EXAMPLE", "")).error() ==
        StsParseError::MissingField);
  CHECK(parse(replace(good, "IEXAMPLE</SessionToken>",
                      std::string(3000, 'A') + "</SessionToken>"))
            .error() == StsParseError::FieldTooLong);
  CHECK(parse(replace(good, "2026-10-27T10", "2026-02-30T10")).error() ==
        StsParseError::BadExpiration);
  CHECK(parse(replace(good, "</Credentials>",
                      "</Credentials><Credentials></Credentials>"))
            .error() == StsParseError::Malformed);
  CHECK(parse(replace(good, "<Credentials>", "<Credentials><Credentials>"))
            .error() == StsParseError::Malformed);
  CHECK(parse(replace(good, "ad4156e9", std::string("a\0b", 3))).error() ==
        StsParseError::Malformed);
  CHECK(parse(good + std::string(kStsDenied)).error() ==
        StsParseError::Malformed);
  CHECK(parse(good + std::string(9000, ' ')).error() ==
        StsParseError::Malformed);
  CHECK(parse(std::string(kStsDenied)).error() == StsParseError::Rejected);
  CHECK(parse(std::string(kStsDenied.substr(0, 60))).error() ==
        StsParseError::Truncated);
  CHECK(error_code(kStsDenied) == "InvalidIdentityToken");
  CHECK(error_code("<Error><Code>ExpiredToken</Code></Error>") ==
        "ExpiredToken");
  CHECK(error_code("<Code>").empty());
  CHECK(error_code("<Code></Code>").empty());
  CHECK(error_code("<Code>a b</Code>").empty());
  CHECK(error_code("<Code>" + std::string(65, 'A') + "</Code>").empty());

  // Flipping any single byte never yields credentials that differ from the
  // ones in the document, and never reads out of bounds (ASan).
  for (std::size_t i = 0; i < good.size(); ++i) {
    for (const char c : {'<', '>', '/', '&', '\0', 'Z'}) {
      std::string hostile = good;
      hostile[i] = c;
      Credentials out;
      if (parse_sts_response(hostile, out)) {
        CHECK(out.present());
        CHECK(hostile.find(out.key_id()) != std::string::npos);
        CHECK(hostile.find(out.token()) != std::string::npos);
      }
    }
  }

  CHECK(parse_iso8601("2026-10-27T10:00:00Z").value() == 1793095200);
  CHECK(parse_iso8601("2026-10-27T10:00:00.5Z").value() == 1793095200);
  CHECK(!parse_iso8601("2026-10-27T10:00:00.Z"));
  CHECK(!parse_iso8601("2026-10-27T24:00:00Z"));
  CHECK(!parse_iso8601("2019-12-31T23:59:59Z"));
  CHECK(!parse_iso8601("2026-10-27 10:00:00Z"));
  CHECK(!parse_iso8601("2026-10-27T10:00:00+00:00"));
}

constexpr std::string_view kThing =
    "thing=8d5b4f8e-0f6b-4b3e-9c55-2b8d0e7c1a10";
std::string dated(int day, int hhmm, int attempt = 0) {
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "%.*s/year=2026/month=10/day=%02d/"
                "data_%04d_0123456789abcdef0123456789abcdef_0-59-%d.parquet",
                static_cast<int>(kThing.size()), kThing.data(), day, hhmm,
                attempt);
  return buf;
}

struct SetLedger : LedgerView {
  std::vector<std::string> names;
  mutable int asked = 0;
  bool contains(std::string_view n) const override {
    ++asked;
    for (const auto &x : names)
      if (x == n)
        return true;
    return false;
  }
};

void test_policy() {
  const Destination dev{.target = Target::Dev,
                        .product = "opensensor-space",
                        .board = "cores3",
                        .prod_prefix = ""};
  std::array<char, kMaxObjectKey> key{};
  const std::string name = dated(27, 905);
  auto n = object_key(dev, name, key);
  CHECK(n.has_value());
  const std::string_view k(key.data(), *n);
  CHECK(k == "opensensor-space/dev/opensensor-space-firmware/cores3/" + name);
  CHECK(key_within_target(dev, k));

  // Dev never writes outside the dev prefix.
  Destination bad = dev;
  bad.prod_prefix = "cores3/";
  CHECK(object_key(bad, name, key).error() == KeyError::BadPrefix);
  CHECK(!key_within_target(dev, "opensensor-space/" + name));
  CHECK(!key_within_target(
      dev, "opensensor-space/dev/opensensor-space-firmware/waveshare/" + name));
  CHECK(!key_within_target(
      dev, "opensensor-space/dev/opensensor-space-firmware/cores3/../" + name));
  CHECK(!key_within_target(dev, "other/dev/opensensor-space-firmware/cores3/" +
                                    name));
  bad = dev;
  bad.board = "Cores3";
  CHECK(object_key(bad, name, key).error() == KeyError::BadBoard);
  bad = dev;
  bad.board = "a/b";
  CHECK(object_key(bad, name, key).error() == KeyError::BadBoard);
  bad = dev;
  bad.product = "";
  CHECK(object_key(bad, name, key).error() == KeyError::BadProduct);

  Destination prod{.target = Target::Prod,
                   .product = "opensensor-space",
                   .board = "cores3",
                   .prod_prefix = "dev/x/"};
  CHECK(object_key(prod, name, key).error() == KeyError::BadPrefix);
  prod.prod_prefix = "";
  CHECK(object_key(prod, name, key).error() == KeyError::BadPrefix);
  prod.prod_prefix = "intake/";
  n = object_key(prod, name, key);
  CHECK(n &&
        std::string_view(key.data(), *n) == "opensensor-space/intake/" + name);
  CHECK(!key_within_target(dev, std::string_view(key.data(), *n)));
  std::array<char, 40> tiny{};
  CHECK(object_key(dev, name, tiny).error() == KeyError::Overflow);

  // Only finalized dated files are eligible.
  CHECK(finalized_dated(name).has_value());
  const auto t = *finalized_dated(name);
  CHECK(t.year == 2026 && t.month == 10 && t.day == 27 && t.hour == 9 &&
        t.minute == 5);
  for (const std::string &reject : {
           std::string(kThing) +
               "/unsynced/boot=ab/data_unsynced_ab_0-3-0.parquet",
           std::string(kThing) + "/year=2026/month=10/day=27/"
                                 "data_0905_ab_42-3-ab-00000007.partial",
           std::string("quarantine/") + name,
           std::string("bench/") + name,
           replace(name, ".parquet", ".parquet.tmp"),
           replace(name, "data_0905", "data_2460"),
           replace(name, "month=10", "month=13"),
           replace(name, "_0123", "_XYZ"),
           replace(name, "year=2026/", "year=2026//"),
           replace(name, "year=2026", "year=../.."),
           replace(name, "-0.parquet", "-0.x.parquet"),
           "/" + name,
       })
    CHECK(!finalized_dated(reject));
  CHECK(object_key(dev, std::string(kThing) + "/unsynced/x.parquet", key)
            .error() == KeyError::BadName);

  // Oldest first, ties by name, ledger asked only for a better candidate.
  SetLedger ledger;
  ledger.names = {dated(26, 2300)};
  NextFile next(ledger);
  next.offer(dated(27, 905), 10);
  next.offer(dated(26, 2300), 10);
  next.offer(dated(27, 100, 1), 10);
  next.offer(dated(27, 100, 0), 10);
  next.offer(std::string(kThing) + "/unsynced/boot=ab/data_unsynced_ab_0-3-0."
                                   "parquet",
             10);
  next.offer(dated(28, 0), 0);
  CHECK(next.found());
  CHECK(next.name() == dated(27, 100, 0));
  const int asked = ledger.asked;
  next.offer(dated(28, 0), 10);
  CHECK(ledger.asked == asked);
  SetLedger all;
  all.names = {dated(1, 0)};
  NextFile none(all);
  none.offer(dated(1, 0), 5);
  CHECK(!none.found());

  // Gate order.
  PolicyState s;
  Inputs in{.enabled = true,
            .clock = ClockSource::Network,
            .network_up = true,
            .mono_ms = 1000,
            .key_generation = 1};
  CHECK(decide(s, in) == Gate::Run);
  Inputs x = in;
  x.enabled = false;
  CHECK(decide(s, x) == Gate::Disabled);
  for (const ClockSource c : {ClockSource::None, ClockSource::Rtc}) {
    x = in;
    x.clock = c;
    CHECK(decide(s, x) == Gate::ClockUntrusted);
  }
  x = in;
  x.clock = ClockSource::Host;
  CHECK(decide(s, x) == Gate::Run);
  x = in;
  x.network_up = false;
  CHECK(decide(s, x) == Gate::Offline);
  // Wi-Fi up never stands in for a trusted clock.
  x = in;
  x.clock = ClockSource::None;
  x.network_up = true;
  CHECK(decide(s, x) == Gate::ClockUntrusted);

  // Backoff grows and caps.
  CHECK(backoff_ms(0) == 0);
  CHECK(backoff_ms(1) == 30000);
  CHECK(backoff_ms(2) == 60000);
  CHECK(backoff_ms(3) == 120000);
  CHECK(backoff_ms(8) == 3600000);
  CHECK(backoff_ms(4000000000U) == 3600000);
  apply(s, Outcome::Transient, in);
  CHECK(s.failures == 1 && s.retry_at_ms == 31000);
  CHECK(decide(s, in) == Gate::BackingOff);
  x = in;
  x.mono_ms = 31000;
  CHECK(decide(s, x) == Gate::Run);
  apply(s, Outcome::Transient, x);
  CHECK(s.failures == 2 && s.retry_at_ms == 91000);
  apply(s, Outcome::Uploaded, x);
  CHECK(s.failures == 0 && decide(s, x) == Gate::Run);

  // 401 and 403 stop until the key changes, and stay stopped past backoff.
  for (const Outcome o : {Outcome::AuthRejected, Outcome::RequestRejected,
                          Outcome::OutsideTarget}) {
    PolicyState st;
    apply(st, o, in);
    CHECK(decide(st, in) == Gate::Stopped);
    x = in;
    x.mono_ms = 1ULL << 40;
    CHECK(decide(st, x) == Gate::Stopped);
    x.key_generation = 2;
    CHECK(decide(st, x) == Gate::Run);
    apply(st, Outcome::Uploaded, x);
    CHECK(!st.stopped);
  }
  x = in;
  x.enabled = false;
  PolicyState st;
  apply(st, Outcome::AuthRejected, in);
  CHECK(decide(st, x) == Gate::Disabled);

  CHECK(classify_put(200, "") == PutClass::Created);
  CHECK(classify_put(412, "PreconditionFailed") == PutClass::Exists);
  CHECK(classify_put(403, "ExpiredToken") == PutClass::Expired);
  CHECK(classify_put(400, "ExpiredToken") == PutClass::Expired);
  CHECK(classify_put(403, "AccessDenied") == PutClass::Auth);
  CHECK(classify_put(401, "") == PutClass::Auth);
  CHECK(classify_put(0, "") == PutClass::Transient);
  CHECK(classify_put(408, "") == PutClass::Transient);
  CHECK(classify_put(429, "") == PutClass::Transient);
  CHECK(classify_put(500, "") == PutClass::Transient);
  CHECK(classify_put(503, "SlowDown") == PutClass::Transient);
  CHECK(classify_put(400, "BadDigest") == PutClass::Rejected);
  CHECK(classify_put(404, "NoSuchBucket") == PutClass::Rejected);
  CHECK(classify_put(301, "") == PutClass::Rejected);
  CHECK(classify_sts_failure(403) == Outcome::AuthRejected);
  CHECK(classify_sts_failure(400) == Outcome::AuthRejected);
  CHECK(classify_sts_failure(429) == Outcome::Transient);
  CHECK(classify_sts_failure(502) == Outcome::Transient);
  CHECK(classify_sts_failure(200) == Outcome::Transient);
}

void test_ledger() {
  const std::string name = dated(27, 905);
  CHECK(sanitize_etag("\"9b2cf535f27731c974343645a3985328\"") ==
        "9b2cf535f27731c974343645a3985328");
  CHECK(sanitize_etag("W/\"abc-2\"") == "abc-2");
  CHECK(sanitize_etag("") == "-");
  CHECK(sanitize_etag("\"a b\"") == "-");
  CHECK(sanitize_etag("a\nb") == "-");
  CHECK(sanitize_etag(std::string(65, 'a')) == "-");

  std::array<char, kLedgerLineMax> line{};
  const auto n = format_ledger_line(
      {.name = name, .bytes = 123456, .crc32 = 0xcbf43926, .etag = "abc-2"},
      line);
  CHECK(n.has_value());
  const std::string text(line.data(), *n);
  CHECK(text.starts_with(name + " 123456 cbf43926 abc-2 "));
  CHECK(text.back() == '\n');
  const auto e = parse_ledger_line(std::string_view(text).substr(0, *n - 1));
  CHECK(e && e->name == name && e->bytes == 123456 && e->crc32 == 0xcbf43926 &&
        e->etag == "abc-2");
  // Any single byte change fails the line checksum or the field rules.
  for (std::size_t i = 0; i + 1 < text.size(); ++i) {
    std::string flipped = text.substr(0, text.size() - 1);
    flipped[i] = static_cast<char>(flipped[i] ^ 0x01);
    CHECK(!parse_ledger_line(flipped));
  }
  CHECK(
      format_ledger_line({.name = "../x.parquet", .etag = "-"}, line).error() ==
      LedgerError::BadName);
  CHECK(format_ledger_line({.name = name, .etag = "a b"}, line).error() ==
        LedgerError::BadEtag);
  std::array<char, 20> small{};
  CHECK(format_ledger_line({.name = name, .etag = "-"}, small).error() ==
        LedgerError::Overflow);

  std::array<char, 256> path{};
  const auto p = ledger_day_path(name, path);
  CHECK(p && std::string_view(path.data(), *p) ==
                 "upload/ledger/" + std::string(kThing) +
                     "/year=2026/month=10/day=27.log");
  CHECK(!ledger_day_path("x.parquet", path));

  // Chunked scans agree for every chunk size, torn and bad lines skipped.
  std::string file;
  for (int i = 0; i < 5; ++i) {
    const auto m = format_ledger_line(
        {.name = dated(27, i), .bytes = 1, .crc32 = 2, .etag = "-"}, line);
    CHECK(m.has_value());
    file.append(line.data(), *m);
  }
  file += "garbage line\n";
  file += std::string(kLedgerLineMax + 50, 'x') + "\n";
  file += "\n";
  const auto torn = format_ledger_line(
      {.name = dated(27, 9), .bytes = 1, .crc32 = 2, .etag = "-"}, line);
  CHECK(torn.has_value());
  file.append(line.data(), *torn - 5);
  for (std::size_t chunk = 1; chunk <= file.size(); chunk += 7) {
    for (const std::string &want : {dated(27, 3), dated(27, 9), dated(27, 7)}) {
      LedgerScan scan(want);
      for (std::size_t at = 0; at < file.size(); at += chunk)
        scan.feed(std::span<const char>(file.data() + at,
                                        std::min(chunk, file.size() - at)));
      scan.finish();
      CHECK(scan.found() == (want == dated(27, 3)));
      CHECK(scan.good_lines() == 5);
      CHECK(scan.bad_lines() == 3);
      CHECK(scan.torn_tail());
    }
  }
  // A complete line without its newline is still torn, never trusted.
  {
    const auto whole = format_ledger_line(
        {.name = name, .bytes = 1, .crc32 = 2, .etag = "-"}, line);
    CHECK(whole.has_value());
    LedgerScan scan(name);
    scan.feed(std::span<const char>(line.data(), *whole - 1));
    scan.finish();
    CHECK(!scan.found());
    CHECK(scan.torn_tail());
    CHECK(scan.good_lines() == 0);
    CHECK(scan.bad_lines() == 1);
  }
}

// ---- Session against fakes ----------------------------------------------

struct FakeArchive final : Archive {
  std::map<std::string, std::string> files;
  bool fail_list = false;
  int reads = 0;
  bool list(NextFile &next) override {
    if (fail_list)
      return false;
    for (const auto &[name, data] : files)
      next.offer(name, data.size());
    return true;
  }
  std::expected<std::size_t, IoError>
  read(std::string_view name, std::uint64_t offset,
       std::span<std::uint8_t> out) override {
    ++reads;
    const auto it = files.find(std::string(name));
    if (it == files.end() || offset > it->second.size())
      return std::unexpected(IoError::Failed);
    const std::size_t n =
        std::min<std::size_t>(out.size(), it->second.size() - offset);
    std::memcpy(out.data(), it->second.data() + offset, n);
    return n;
  }
};

// Stores the ledger as the real text format and scans it like the device.
struct FakeLedger final : Ledger {
  std::string text;
  bool fail_append = false;
  bool contains(std::string_view name) const override {
    LedgerScan scan(name);
    scan.feed(text);
    scan.finish();
    return scan.found();
  }
  bool append(const LedgerEntry &entry) override {
    if (fail_append)
      return false;
    if (!text.empty() && text.back() != '\n')
      text += '\n';
    std::array<char, kLedgerLineMax> line{};
    const auto n = format_ledger_line(entry, line);
    if (!n)
      return false;
    text.append(line.data(), *n);
    return true;
  }
};

constexpr std::string_view kApiKey = "<sck-key>";

struct FakeCloud final : Transport {
  std::map<std::string, std::string> objects;
  int sts_calls = 0;
  int put_calls = 0;
  int sts_status = 200;
  std::string sts_body = std::string(kStsOk);
  int put_status = 0; // 0 means behave like S3
  std::string put_body;
  bool fail_transport = false;
  std::vector<std::string> seen; // every path and header value sent

  std::expected<HttpResponse, TransportError>
  send(const HttpRequest &r, std::span<char> out) override {
    if (fail_transport)
      return std::unexpected(TransportError::Connect);
    seen.emplace_back(r.path);
    for (const Header &h : r.headers) {
      seen.emplace_back(h.name);
      seen.emplace_back(h.value);
    }
    CHECK(r.host == "data.source.coop");
    std::string body;
    std::array<std::uint8_t, 7> buf{};
    while (body.size() < r.body_bytes) {
      const auto got = r.body->read(body.size(), buf);
      if (!got || *got == 0)
        return std::unexpected(TransportError::Body);
      body.append(reinterpret_cast<const char *>(buf.data()), *got);
    }
    HttpResponse resp;
    const auto reply = [&](int status, std::string_view text) {
      resp.status = status;
      resp.body_bytes = std::min(text.size(), out.size());
      resp.truncated = text.size() > out.size();
      std::memcpy(out.data(), text.data(), resp.body_bytes);
      return resp;
    };
    if (r.method == "POST") {
      ++sts_calls;
      CHECK(r.path == "/.sts");
      CHECK(body.find("WebIdentityToken=%3Csck-key%3E") != std::string::npos);
      return reply(sts_status, sts_body);
    }
    ++put_calls;
    CHECK(r.method == "PUT");
    const std::string prefix =
        "/walkthru-earth/opensensor-space/dev/opensensor-space-firmware/"
        "cores3/thing%3D";
    CHECK(std::string_view(r.path).starts_with(prefix));
    bool if_none_match = false;
    bool token = false;
    bool auth = false;
    for (const Header &h : r.headers) {
      if_none_match |= h.name == "if-none-match" && h.value == "*";
      token |= h.name == "x-amz-security-token" &&
               h.value == "AQoDYXdzEE0a8ANXXXXXXXXNO1ewxE5TijQyp+IEXAMPLE";
      auth |= h.name == "authorization" &&
              h.value.starts_with("AWS4-HMAC-SHA256 Credential="
                                  "ASIAIOSFODNN7EXAMPLE/20261027/us-west-2/s3/"
                                  "aws4_request, SignedHeaders=content-type;"
                                  "host;if-none-match;x-amz-content-sha256;"
                                  "x-amz-date;x-amz-security-token, "
                                  "Signature=");
    }
    CHECK(if_none_match && token && auth);
    if (put_status != 0)
      return reply(put_status, put_body);
    if (objects.contains(std::string(r.path)))
      return reply(412, "<Error><Code>PreconditionFailed</Code></Error>");
    objects[std::string(r.path)] = body;
    resp = reply(200, "");
    constexpr std::string_view etag = "\"9b2cf535f27731c974343645a3985328\"";
    std::memcpy(resp.etag.data(), etag.data(), etag.size());
    resp.etag_bytes = etag.size();
    return resp;
  }
};

const SessionConfig kConfig{
    .host = "data.source.coop",
    .bucket = "walkthru-earth",
    .region = "us-west-2",
    .destination = {.target = Target::Dev,
                    .product = "opensensor-space",
                    .board = "cores3",
                    .prod_prefix = ""},
    .role_arn = "arn:aws:iam::example--board:role/FullAccess",
    .session_name = "cores3",
    .api_key = kApiKey,
};

// 2026-10-27T09:05:07Z, inside the STS credential lifetime above.
constexpr std::int64_t kNow = 1793091907;

Inputs running(std::uint64_t mono_ms = 1000, std::uint32_t generation = 1) {
  return {.enabled = true,
          .clock = ClockSource::Network,
          .network_up = true,
          .mono_ms = mono_ms,
          .key_generation = generation};
}

FakeArchive sample_archive() {
  FakeArchive a;
  a.files[dated(27, 905)] = std::string(3000, 'b');
  a.files[dated(26, 2300)] = std::string(2500, 'a');
  a.files[std::string(kThing) + "/unsynced/boot=ab/data_unsynced_ab_0-3-0."
                                "parquet"] = "u";
  a.files[std::string(kThing) +
          "/year=2026/month=10/day=27/data_1000_ab_1-2-ab-00000001.partial"] =
      "p";
  return a;
}

void test_session_happy_and_rerun() {
  RefSha256 hash;
  FakeArchive archive = sample_archive();
  const auto before = archive.files;
  FakeLedger ledger;
  FakeCloud cloud;
  {
    Session s(kConfig, hash, cloud, archive, ledger);
    auto r = s.step(running(), kNow);
    CHECK(r.gate == Gate::Run && r.outcome == Outcome::Uploaded);
    CHECK(cloud.sts_calls == 1 && cloud.put_calls == 1);
    // Oldest first.
    CHECK(cloud.objects.begin()->first.ends_with(
        "day%3D26/data_2300_0123456789abcdef0123456789abcdef_0-59-0.parquet"));
    CHECK(cloud.objects.begin()->second == std::string(2500, 'a'));
    r = s.step(running(2000), kNow + 60);
    CHECK(r.outcome == Outcome::Uploaded);
    CHECK(cloud.sts_calls == 1 && cloud.put_calls == 2);
    r = s.step(running(3000), kNow + 120);
    CHECK(r.outcome == Outcome::NothingPending);
    CHECK(cloud.put_calls == 2);
  }
  CHECK(ledger.text.find(dated(26, 2300) + " 2500 ") == 0);
  CHECK(ledger.text.find(" 9b2cf535f27731c974343645a3985328 ") !=
        std::string::npos);
  // A re-run, even after a reboot, uploads nothing new.
  {
    Session again(kConfig, hash, cloud, archive, ledger);
    const auto r = again.step(running(), kNow);
    CHECK(r.outcome == Outcome::NothingPending);
    CHECK(cloud.put_calls == 2 && cloud.sts_calls == 1);
  }
  // The archive is never deleted, renamed or relabelled.
  CHECK(archive.files == before);
  // The API key only ever travels in the STS body.
  for (const std::string &s : cloud.seen)
    CHECK(s.find("sck") == std::string::npos);
}

void test_session_lost_ledger_records_412() {
  RefSha256 hash;
  FakeArchive archive = sample_archive();
  FakeLedger first;
  FakeCloud cloud;
  Session a(kConfig, hash, cloud, archive, first);
  CHECK(a.step(running(), kNow).outcome == Outcome::Uploaded);
  // The ledger is gone. The server says 412 and the ledger learns it.
  FakeLedger fresh;
  Session b(kConfig, hash, cloud, archive, fresh);
  CHECK(b.step(running(), kNow).outcome == Outcome::AlreadyUploaded);
  CHECK(fresh.text.find(dated(26, 2300) + " 2500 ") == 0);
  CHECK(fresh.text.find(" - ") != std::string::npos);
  CHECK(b.step(running(2000), kNow).outcome == Outcome::Uploaded);
  CHECK(b.step(running(3000), kNow).outcome == Outcome::NothingPending);
  // A failed append is retried, and the retry is a 412.
  FakeLedger flaky;
  flaky.fail_append = true;
  Session c(kConfig, hash, cloud, archive, flaky);
  CHECK(c.step(running(), kNow).outcome == Outcome::Transient);
  flaky.fail_append = false;
  CHECK(c.step(running(1000 + 30000), kNow).outcome ==
        Outcome::AlreadyUploaded);
  // A torn last line is fenced off by the next append.
  FakeLedger torn;
  torn.text = "thing=x/year=2026/month=10/day=26/data_23";
  Session d(kConfig, hash, cloud, archive, torn);
  CHECK(d.step(running(), kNow).outcome == Outcome::AlreadyUploaded);
  CHECK(torn.contains(dated(26, 2300)));
}

void test_session_gates() {
  RefSha256 hash;
  FakeArchive archive = sample_archive();
  FakeLedger ledger;
  FakeCloud cloud;
  Session s(kConfig, hash, cloud, archive, ledger);
  Inputs in = running();
  in.enabled = false;
  CHECK(s.step(in, kNow).gate == Gate::Disabled);
  in = running();
  in.clock = ClockSource::Rtc;
  CHECK(s.step(in, kNow).gate == Gate::ClockUntrusted);
  in = running();
  in.network_up = false;
  CHECK(s.step(in, kNow).gate == Gate::Offline);
  CHECK(cloud.sts_calls == 0 && cloud.put_calls == 0 && archive.reads == 0);
}

void test_session_auth_stops() {
  RefSha256 hash;
  FakeArchive archive = sample_archive();
  FakeLedger ledger;
  {
    // A revoked key, STS says InvalidIdentityToken.
    FakeCloud cloud;
    cloud.sts_status = 400;
    cloud.sts_body = std::string(kStsDenied);
    Session s(kConfig, hash, cloud, archive, ledger);
    CHECK(s.step(running(), kNow).outcome == Outcome::AuthRejected);
    for (int i = 0; i < 50; ++i)
      CHECK(s.step(running(1000 + i * 3600000ULL), kNow).gate == Gate::Stopped);
    CHECK(cloud.sts_calls == 1 && cloud.put_calls == 0);
    // A new key resumes.
    cloud.sts_status = 200;
    cloud.sts_body = std::string(kStsOk);
    CHECK(s.step(running(5000, 2), kNow).outcome == Outcome::Uploaded);
    CHECK(cloud.sts_calls == 2);
  }
  {
    // S3 refuses the credentials. Stop and drop them.
    FakeCloud cloud;
    cloud.put_status = 403;
    cloud.put_body = "<Error><Code>AccessDenied</Code></Error>";
    Session s(kConfig, hash, cloud, archive, ledger);
    CHECK(s.step(running(), kNow).outcome == Outcome::AuthRejected);
    CHECK(!s.has_credentials());
    CHECK(s.step(running(1ULL << 40), kNow).gate == Gate::Stopped);
    CHECK(cloud.put_calls == 1);
  }
  {
    // Expired credentials refresh after a backoff, never stop.
    FakeCloud cloud;
    cloud.put_status = 403;
    cloud.put_body = "<Error><Code>ExpiredToken</Code></Error>";
    FakeLedger l;
    Session s(kConfig, hash, cloud, archive, l);
    CHECK(s.step(running(), kNow).outcome == Outcome::Transient);
    CHECK(!s.has_credentials());
    CHECK(s.step(running(2000), kNow).gate == Gate::BackingOff);
    cloud.put_status = 0;
    CHECK(s.step(running(1000 + 30000), kNow).outcome == Outcome::Uploaded);
    CHECK(cloud.sts_calls == 2);
  }
  {
    // A 400 the server will never accept stops instead of looping.
    FakeCloud cloud;
    cloud.put_status = 400;
    cloud.put_body = "<Error><Code>BadDigest</Code></Error>";
    FakeLedger l;
    Session s(kConfig, hash, cloud, archive, l);
    CHECK(s.step(running(), kNow).outcome == Outcome::RequestRejected);
    CHECK(s.step(running(1ULL << 40), kNow).gate == Gate::Stopped);
  }
}

void test_session_transient_and_refresh() {
  RefSha256 hash;
  FakeArchive archive = sample_archive();
  FakeLedger ledger;
  FakeCloud cloud;
  Session s(kConfig, hash, cloud, archive, ledger);
  cloud.fail_transport = true;
  CHECK(s.step(running(), kNow).outcome == Outcome::Transient);
  CHECK(s.step(running(2000), kNow).gate == Gate::BackingOff);
  cloud.fail_transport = false;
  cloud.put_status = 503;
  CHECK(s.step(running(31000), kNow).outcome == Outcome::Transient);
  CHECK(s.state().failures == 2);
  CHECK(s.step(running(31000 + 59999), kNow).gate == Gate::BackingOff);
  cloud.put_status = 0;
  CHECK(s.step(running(31000 + 60000), kNow).outcome == Outcome::Uploaded);
  CHECK(s.state().failures == 0);
  const int sts = cloud.sts_calls;
  // Close to expiry the credentials refresh before the next PUT.
  CHECK(s.step(running(200000), 1793095200 - 200).outcome ==
        Outcome::Transient); // the fresh STS reply also expires too soon
  CHECK(cloud.sts_calls == sts + 1);
  archive.fail_list = true;
  CHECK(s.step(running(1ULL << 30), kNow).outcome == Outcome::Transient);
}

void test_session_config_guard() {
  RefSha256 hash;
  FakeArchive archive = sample_archive();
  FakeLedger ledger;
  FakeCloud cloud;
  SessionConfig bad = kConfig;
  bad.destination.prod_prefix = "elsewhere/";
  Session s(bad, hash, cloud, archive, ledger);
  CHECK(s.step(running(), kNow).outcome == Outcome::RequestRejected);
  CHECK(s.step(running(1ULL << 40), kNow).gate == Gate::Stopped);
  CHECK(cloud.sts_calls == 0 && cloud.put_calls == 0);
}

} // namespace

int main() {
  test_sha256_and_hmac();
  test_sigv4_vectors();
  test_sigv4_rejects();
  test_dates_and_encoding();
  test_sts();
  test_policy();
  test_ledger();
  test_session_happy_and_rerun();
  test_session_lost_ledger_records_412();
  test_session_gates();
  test_session_auth_stops();
  test_session_transient_and_refresh();
  test_session_config_guard();
  std::printf("upload: %d checks passed\n", checks);
  return 0;
}
