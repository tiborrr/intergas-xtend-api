// UART framing between BoilerBridge_Client and BoilerBridge_Server.
//
// Frame: A5 5A | type | id | len (LE16) | payload | crc16 (LE16, over type..payload)
// Plain C++ without Arduino, so tests/test_bridge_link.cpp can run on a computer.
// Each sketch folder links to this file: both boards must speak the same version.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bridge {

enum FrameType : uint8_t {
  kStats = 1,     // client -> server: JSON body from the stats poll
  kRequest = 2,   // server -> client: "METHOD /path\nName: value\n...", for a POST followed by "\nbody"
  kRespHead = 3,  // client -> server: "code\nName: value\n..."
  kRespData = 4,  // client -> server: body chunk, answered with kAck
  kRespEnd = 5,   // client -> server: 0 = complete, 1 = aborted
  kAck = 6,       // server -> client: stats, head or chunk handled; client -> server: OTA chunk written
  kOtaBegin = 7,  // server -> client: start a firmware update
  kOtaData = 8,   // server -> client: firmware chunk, answered with kAck
  kOtaEnd = 9,    // server -> client: all sent, verify and install
  kOtaResult = 10,  // client -> server: 0 = installed, restarting; 1 = failed
};

static const uint8_t kMagic0 = 0xA5;
static const uint8_t kMagic1 = 0x5A;
static const size_t kHeaderSize = 6;
static const size_t kCrcSize = 2;
// One body chunk. The sender waits for kAck after each, so this also bounds
// what can pile up in the receiver's UART buffer.
static const size_t kChunkSize = 1024;

// The only path the passthrough may POST to: the dashboard's service login. An
// exact match, so settings (/api/params/values) can't be written through the bridge.
inline bool isPostAllowed(const char *path) { return std::strcmp(path, "/api/auth") == 0; }

// Cuts a request frame's text at the blank line and returns the body after it,
// or nullptr when it has none.
inline char *splitRequestBody(char *text) {
  char *gap = std::strstr(text, "\n\n");
  if (gap == nullptr) {
    return nullptr;
  }
  gap[1] = '\0';
  return gap + 2;
}

inline uint16_t crc16Update(uint16_t crc, uint8_t byte) {  // CRC-16/CCITT-FALSE
  crc ^= static_cast<uint16_t>(byte) << 8;
  for (int i = 0; i < 8; ++i) {
    crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

// Calls write(data, size) for each part of the frame.
template <typename Write>
void writeFrame(Write write, uint8_t type, uint8_t id, const void *payload, size_t len) {
  const uint8_t header[kHeaderSize] = {
      kMagic0, kMagic1, type, id, static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8),
  };
  uint16_t crc = 0xFFFF;
  for (size_t i = 2; i < kHeaderSize; ++i) {
    crc = crc16Update(crc, header[i]);
  }
  const uint8_t *bytes = static_cast<const uint8_t *>(payload);
  for (size_t i = 0; i < len; ++i) {
    crc = crc16Update(crc, bytes[i]);
  }
  const uint8_t trailer[kCrcSize] = {static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>(crc >> 8)};
  write(header, kHeaderSize);
  if (len > 0) {
    write(bytes, len);
  }
  write(trailer, kCrcSize);
}

// Feed it bytes; feed() returns true once a frame passed its CRC. Noise and
// damaged frames are skipped by searching for the next magic.
template <size_t MaxPayload>
class FrameParser {
 public:
  bool feed(uint8_t b) {
    switch (state_) {
      case kWaitMagic0:
        if (b == kMagic0) {
          state_ = kWaitMagic1;
        }
        return false;
      case kWaitMagic1:
        state_ = (b == kMagic1) ? kReadHeader : (b == kMagic0 ? kWaitMagic1 : kWaitMagic0);
        pos_ = 0;
        crc_ = 0xFFFF;
        return false;
      case kReadHeader:
        header_[pos_++] = b;
        crc_ = crc16Update(crc_, b);
        if (pos_ == sizeof(header_)) {
          len_ = static_cast<size_t>(header_[2]) | (static_cast<size_t>(header_[3]) << 8);
          pos_ = 0;
          if (len_ > MaxPayload) {
            ++errors_;
            state_ = kWaitMagic0;
          } else {
            state_ = len_ > 0 ? kReadPayload : kReadCrc0;
          }
        }
        return false;
      case kReadPayload:
        payload_[pos_++] = b;
        crc_ = crc16Update(crc_, b);
        if (pos_ == len_) {
          state_ = kReadCrc0;
        }
        return false;
      case kReadCrc0:
        crcLow_ = b;
        state_ = kReadCrc1;
        return false;
      case kReadCrc1: {
        state_ = kWaitMagic0;
        const uint16_t received = static_cast<uint16_t>(crcLow_) | (static_cast<uint16_t>(b) << 8);
        if (received != crc_) {
          ++errors_;
          return false;
        }
        payload_[len_] = 0;  // text payloads can be used as C strings
        return true;
      }
    }
    return false;
  }

  uint8_t type() const { return header_[0]; }
  uint8_t id() const { return header_[1]; }
  size_t length() const { return len_; }
  char *payload() { return reinterpret_cast<char *>(payload_); }
  uint32_t errors() const { return errors_; }

 private:
  enum State { kWaitMagic0, kWaitMagic1, kReadHeader, kReadPayload, kReadCrc0, kReadCrc1 };

  State state_ = kWaitMagic0;
  uint8_t header_[4] = {0, 0, 0, 0};  // type, id, len
  uint8_t payload_[MaxPayload + 1];
  size_t pos_ = 0;
  size_t len_ = 0;
  uint16_t crc_ = 0;
  uint8_t crcLow_ = 0;
  uint32_t errors_ = 0;
};

}  // namespace bridge
