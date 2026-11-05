// Runs on a computer, no hardware needed:
//   c++ -std=c++17 -Ifirmware/shared tests/test_bridge_link.cpp -o /tmp/test_bridge_link && /tmp/test_bridge_link

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bridge_link.h"

using namespace bridge;
using Parser = FrameParser<2048>;

static std::vector<uint8_t> encode(uint8_t type, uint8_t id, const std::string &payload) {
  std::vector<uint8_t> out;
  writeFrame([&](const uint8_t *data, size_t n) { out.insert(out.end(), data, data + n); }, type, id, payload.data(),
             payload.size());
  return out;
}

static int feedAll(Parser &parser, const std::vector<uint8_t> &bytes) {
  int frames = 0;
  for (uint8_t b : bytes) {
    frames += parser.feed(b) ? 1 : 0;
  }
  return frames;
}

static void test_known_crc() {
  // CRC-16/CCITT-FALSE check value for "123456789".
  uint16_t crc = 0xFFFF;
  for (const char *p = "123456789"; *p; ++p) {
    crc = crc16Update(crc, static_cast<uint8_t>(*p));
  }
  assert(crc == 0x29B1);
}

static void test_round_trip() {
  Parser parser;
  assert(feedAll(parser, encode(kRequest, 7, "GET /index.html\nAccept: */*\n")) == 1);
  assert(parser.type() == kRequest);
  assert(parser.id() == 7);
  assert(std::string(parser.payload()) == "GET /index.html\nAccept: */*\n");
}

static void test_empty_payload() {
  Parser parser;
  assert(feedAll(parser, encode(kAck, 200, "")) == 1);
  assert(parser.type() == kAck && parser.id() == 200 && parser.length() == 0);
}

static void test_binary_payload_containing_magic() {
  std::string body("\xA5\x5A\x01\x02\x00\xFF\xA5\x5A", 8);
  Parser parser;
  assert(feedAll(parser, encode(kRespData, 1, body)) == 1);
  assert(parser.length() == body.size());
  assert(std::memcmp(parser.payload(), body.data(), body.size()) == 0);
}

static void test_skips_leading_noise() {
  std::vector<uint8_t> bytes = {0x00, 0xA5, 0x13, 0xA5, 0xA5};
  auto frame = encode(kStats, 2, "{}");
  bytes.insert(bytes.end(), frame.begin() + 1, frame.end());  // 0xA5 is already there
  Parser parser;
  assert(feedAll(parser, bytes) == 1);
  assert(std::string(parser.payload()) == "{}");
}

static void test_damaged_frame_is_rejected_and_next_one_survives() {
  auto bad = encode(kRespData, 3, "hello");
  bad[8] ^= 0x01;
  Parser parser;
  assert(feedAll(parser, bad) == 0);
  assert(parser.errors() == 1);
  assert(feedAll(parser, encode(kRespData, 4, "world")) == 1);
  assert(parser.id() == 4);
}

static void test_every_single_bit_flip_is_detected() {
  const auto good = encode(kRespData, 9, "some body bytes");
  for (size_t byte = 2; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto bad = good;
      bad[byte] ^= static_cast<uint8_t>(1 << bit);
      Parser parser;
      assert(feedAll(parser, bad) == 0);
    }
  }
}

static void test_oversized_length_is_rejected() {
  Parser parser;
  const std::vector<uint8_t> header = {kMagic0, kMagic1, kRespData, 1, 0xFF, 0xFF};
  assert(feedAll(parser, header) == 0);
  assert(parser.errors() == 1);
  assert(feedAll(parser, encode(kAck, 1, "")) == 1);
}

static void test_largest_payload_fits() {
  const std::string body(2048, 'x');
  Parser parser;
  assert(feedAll(parser, encode(kStats, 1, body)) == 1);
  assert(parser.length() == 2048 && parser.payload()[2048] == 0);
}

static void test_back_to_back_frames() {
  auto bytes = encode(kRespHead, 1, "200\n");
  auto more = encode(kRespEnd, 1, "\x00");
  bytes.insert(bytes.end(), more.begin(), more.end());
  Parser parser;
  assert(feedAll(parser, bytes) == 2);
  assert(parser.type() == kRespEnd);
}

static void test_only_the_service_login_may_be_posted_to() {
  assert(isPostAllowed("/api/auth"));
  for (const char *path : {"/api/params/values", "/api/set_block", "/api/schedules", "/api/auth/", "/api/auth?x=1",
                           "/api/authx", "/API/AUTH", "/xtend/api/auth", "/", ""}) {
    assert(!isPostAllowed(path));
  }
}

static void test_request_body_is_split_off() {
  char request[] = "POST /api/auth\nContent-Type: text/plain\n\n{\"code\":1}";
  const char *body = splitRequestBody(request);
  assert(body != nullptr && std::string(body) == "{\"code\":1}");
  assert(std::string(request) == "POST /api/auth\nContent-Type: text/plain\n");
}

static void test_request_without_body() {
  char request[] = "GET /index.html\nAccept: */*\n";
  assert(splitRequestBody(request) == nullptr);
  assert(std::string(request) == "GET /index.html\nAccept: */*\n");
}

static void test_body_may_hold_a_blank_line() {
  char request[] = "POST /api/auth\n\na\n\nb";
  const char *body = splitRequestBody(request);
  assert(body != nullptr && std::string(body) == "a\n\nb");
  assert(std::string(request) == "POST /api/auth\n");
}

int main() {
  test_only_the_service_login_may_be_posted_to();
  test_request_body_is_split_off();
  test_request_without_body();
  test_body_may_hold_a_blank_line();
  test_known_crc();
  test_round_trip();
  test_empty_payload();
  test_binary_payload_containing_magic();
  test_skips_leading_noise();
  test_damaged_frame_is_rejected_and_next_one_survives();
  test_every_single_bit_flip_is_detected();
  test_oversized_length_is_rejected();
  test_largest_payload_fits();
  test_back_to_back_frames();
  std::puts("ok");
  return 0;
}
