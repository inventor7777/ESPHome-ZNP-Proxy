#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::znp_proxy {

// ZNP MT frames have a one-byte payload length and a five-byte envelope.
struct Frame {
  std::array<uint8_t, 260> bytes{};
  size_t size{0};

  bool feed(uint8_t byte) {
    if (this->size == 0 && byte != 0xFE)
      return false;
    this->bytes[this->size++] = byte;
    if (this->size < 2 || this->size != size_t(this->bytes[1]) + 5)
      return false;
    uint8_t checksum = 0;
    for (size_t i = 1; i < this->size; i++)
      checksum ^= this->bytes[i];
    if (checksum == 0)
      return true;
    this->size = 0;
    return false;
  }
  void clear() { this->size = 0; }
};

enum class Event { RECOVERY_STARTED, REPLAY_STARTED, RECOVERED, LATE_RESPONSE, FAILED };

class Transport {
 public:
  virtual void send_uart(const uint8_t *data, size_t size) = 0;
  virtual void send_host(const uint8_t *data, size_t size) = 0;
  virtual void event(Event event) = 0;
  virtual ~Transport() = default;
};

class Protocol {
 public:
  explicit Protocol(Transport &transport) : transport_(transport) {}
  void set_response_timeout(uint32_t ms) { this->timeout_ms_ = ms; }
  bool paused() const { return this->state_ != State::IDLE; }
  bool failed() const { return this->state_ == State::FAILED; }
  uint8_t transaction() const { return this->saved_.bytes[19]; }
  uint32_t block_offset() const {
    const auto &b = this->saved_.bytes;
    return uint32_t(b[36]) | (uint32_t(b[37]) << 8) | (uint32_t(b[38]) << 16) | (uint32_t(b[39]) << 24);
  }
  void reset() {
    this->host_.clear();
    this->radio_.clear();
    this->saved_.clear();
    this->pending_ = false;
    this->state_ = State::IDLE;
  }

  void host_data(const uint8_t *data, size_t size, uint32_t now) {
    this->last_host_ms_ = now;
    this->transport_.send_uart(data, size);
    for (size_t i = 0; i < size; i++) {
      if (!this->host_.feed(data[i]))
        continue;
      const auto &b = this->host_.bytes;
      if (b[2] == 0x41 && b[3] == 0x00) {
        this->pending_ = false;
        this->saved_.clear();
      } else if ((b[2] & 0xE0) == 0x20) {
        // Overlapping SREQs make response ownership ambiguous; never recover them.
        bool overlap = this->pending_;
        this->pending_ = true;
        this->command0_ = b[2] | 0x40;
        this->command1_ = b[3];
        this->started_ms_ = now;
        this->saved_.clear();
        if (!overlap && is_ota_block(this->host_))
          this->saved_ = this->host_;
      }
      this->host_.clear();
    }
  }

  void radio_byte(uint8_t byte, uint32_t now) {
    this->last_radio_ms_ = now;
    if (!this->radio_.feed(byte))
      return;
    const auto &b = this->radio_.bytes;
    bool internal_ping = (this->state_ == State::PING || this->state_ == State::DRAIN_PING) &&
                         b[2] == 0x61 && b[3] == 0x01;
    if (internal_ping) {
      if (b[1] != 2) {
        this->fail_();
      } else if (this->state_ == State::DRAIN_PING) {
        this->state_ = State::IDLE;
      } else {
        this->state_ = State::REPLAY;
        this->started_ms_ = now;
        this->transport_.event(Event::REPLAY_STARTED);
        this->transport_.send_uart(this->saved_.bytes.data(), this->saved_.size);
      }
    } else {
      this->transport_.send_host(b.data(), this->radio_.size);
      if (b[2] == 0x41 && b[3] == 0x80) {
        if (this->paused())
          this->fail_();
        this->pending_ = false;
        this->saved_.clear();
      } else if (this->pending_ && b[2] == this->command0_ && b[3] == this->command1_) {
        this->pending_ = false;
        this->saved_.clear();
        if (this->state_ == State::REPLAY) {
          this->state_ = State::IDLE;
          this->transport_.event(Event::RECOVERED);
        } else if (this->state_ == State::PING) {
          // Consume the ping reply before another host ping can own it.
          this->state_ = State::DRAIN_PING;
          this->transport_.event(Event::LATE_RESPONSE);
        } else if (this->state_ == State::PADDING) {
          this->state_ = State::IDLE;
          this->transport_.event(Event::LATE_RESPONSE);
        }
      } else if (this->saved_.size != 0 && b[2] == 0x44 && b[3] == 0x80 && b[1] == 3 &&
                 b[5] == this->saved_.bytes[16] && b[6] == this->saved_.bytes[19]) {
        // During replay, callbacks may precede the SRSP; keep waiting for it.
        if (this->state_ != State::REPLAY) {
          this->saved_.clear();
          if (this->paused())
            this->fail_();
        }
      }
    }
    this->radio_.clear();
  }

  void tick(uint32_t now) {
    // An incomplete incoming frame must not swallow the recovery ping forever.
    if (now - this->last_radio_ms_ >= 100)
      this->radio_.clear();
    if (now - this->last_host_ms_ >= 100)
      this->host_.clear();
    uint32_t elapsed = now - this->started_ms_;
    if (this->state_ == State::IDLE && this->pending_ && this->saved_.size != 0 &&
        elapsed >= this->timeout_ms_) {
      std::array<uint8_t, 260> padding;
      padding.fill(0xEF);
      this->state_ = State::PADDING;
      this->started_ms_ = now;
      this->transport_.event(Event::RECOVERY_STARTED);
      this->transport_.send_uart(padding.data(), padding.size());
    } else if (this->state_ == State::PADDING && elapsed >= 100) {
      static constexpr uint8_t PING[] = {0xFE, 0x00, 0x21, 0x01, 0x20};
      this->state_ = State::PING;
      this->started_ms_ = now;
      this->transport_.send_uart(PING, sizeof(PING));
    } else if ((this->state_ == State::PING || this->state_ == State::DRAIN_PING ||
                this->state_ == State::REPLAY) && elapsed >= this->timeout_ms_) {
      this->fail_();
    }
  }

  static bool is_ota_block(const Frame &frame) {
    const auto &b = frame.bytes;
    if (frame.size < 42 || b[2] != 0x24 || b[3] != 0x02 || b[17] != 0x19 || b[18] != 0x00)
      return false;
    // AF.DataRequestExt has a 20-byte header; only standard, server-to-client ZCL is eligible.
    size_t zcl_size = size_t(b[22]) | (size_t(b[23]) << 8);
    return zcl_size + 25 == frame.size && zcl_size >= 17 && (b[24] & 0xEF) == 0x09 &&
           b[26] == 0x05 && b[27] == 0x00 && b[40] > 0 && zcl_size == size_t(b[40]) + 17;
  }

 protected:
  enum class State { IDLE, PADDING, PING, REPLAY, DRAIN_PING, FAILED };
  void fail_() {
    this->state_ = State::FAILED;
    this->transport_.event(Event::FAILED);
  }

  Transport &transport_;
  Frame host_, radio_, saved_;
  uint32_t started_ms_{0};
  uint32_t last_radio_ms_{0}, last_host_ms_{0};
  uint32_t timeout_ms_{500};
  uint8_t command0_{0}, command1_{0};
  bool pending_{false};
  State state_{State::IDLE};
};

}  // namespace esphome::znp_proxy
