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

enum class Event {
  RECOVERY_STARTED, REPLAY_STARTED, RECOVERED, LATE_RESPONSE, FAILED, RADIO_UNRESPONSIVE,
  COMMAND_TIMEOUT, COMMAND_RESPONSIVE, COMMAND_PROBE_RETRY
};

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
  void set_unresponsive_detection(bool enabled) { this->detect_unresponsive_ = enabled; }
  bool paused() const { return this->state_ != State::IDLE; }
  bool failed() const { return this->state_ == State::FAILED; }
  uint8_t transaction() const { return this->saved_.bytes[19]; }
  uint16_t pending_command() const { return (uint16_t(this->command0_ & ~0x40) << 8) | this->command1_; }
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
    this->unanswered_pings_ = 0;
    this->command_probe_ = false;
    this->command_deadline_ms_ = 0;
  }

  void host_data(const uint8_t *data, size_t size, uint32_t now) {
    this->last_host_ms_ = now;
    this->transport_.send_uart(data, size);
    for (size_t i = 0; i < size; i++) {
      if (!this->host_.feed(data[i]))
        continue;
      const auto &b = this->host_.bytes;
      if (this->detect_unresponsive_ && b[1] == 0 && b[2] == 0x21 && b[3] == 0x01) {
        if (this->unanswered_pings_ < 2)
          this->unanswered_pings_++;
        this->last_ping_ms_ = now;
      }
      if (b[2] == 0x41 && b[3] == 0x00) {
        this->pending_ = false;
        this->saved_.clear();
        this->command_deadline_ms_ = 0;
      } else if ((b[2] & 0xE0) == 0x20) {
        // Overlapping SREQs make response ownership ambiguous; never recover them.
        bool overlap = this->pending_;
        this->pending_ = true;
        this->command0_ = b[2] | 0x40;
        this->command1_ = b[3];
        this->started_ms_ = now;
        this->command_deadline_ms_ = overlap ? 0 : command_deadline(b[2], b[3]);
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
    if ((b[1] == 2 && b[2] == 0x61 && b[3] == 0x01) || (b[2] == 0x41 && b[3] == 0x80))
      this->unanswered_pings_ = 0;
    bool probe_error = this->command_probe_ && b[2] == 0x60 && b[3] == 0x00 && b[1] == 3 &&
                       b[5] == 0x21 && b[6] == 0x01;
    bool internal_ping = (this->state_ == State::PING || this->state_ == State::DRAIN_PING) &&
                         ((b[2] == 0x61 && b[3] == 0x01) || probe_error);
    if (internal_ping) {
      if (b[1] != 2 && !probe_error) {
        this->fail_();
      } else if (this->command_probe_) {
        // Never replay ordinary commands or expose private ping replies to the host.
        this->transport_.event(Event::COMMAND_RESPONSIVE);
        this->state_ = State::FAILED;
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
      } else if (this->pending_ &&
                 ((b[2] == this->command0_ && b[3] == this->command1_) ||
                  (b[2] == 0x60 && b[3] == 0x00 && b[1] == 3 &&
                   b[5] == (this->command0_ & ~0x40) && b[6] == this->command1_))) {
        this->pending_ = false;
        this->saved_.clear();
        this->command_deadline_ms_ = 0;
        if (this->state_ == State::REPLAY) {
          this->state_ = State::IDLE;
          this->transport_.event(Event::RECOVERED);
        } else if (this->state_ == State::PING) {
          // Consume the ping reply before another host ping can own it.
          this->state_ = State::DRAIN_PING;
          this->transport_.event(Event::LATE_RESPONSE);
        } else if (this->state_ == State::PADDING) {
          this->state_ = State::IDLE;
          this->command_probe_ = false;
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
    if (this->detect_unresponsive_ && this->unanswered_pings_ >= 2 &&
        now - this->last_ping_ms_ >= 500 && this->request_reset_(now)) {
      return;
    }
    // An incomplete incoming frame must not swallow the recovery ping forever.
    if (now - this->last_radio_ms_ >= 100)
      this->radio_.clear();
    if (now - this->last_host_ms_ >= 100)
      this->host_.clear();
    uint32_t elapsed = now - this->started_ms_;
    bool ota_timeout = this->saved_.size != 0 && elapsed >= this->timeout_ms_;
    bool command_timeout = this->detect_unresponsive_ && this->command_deadline_ms_ != 0 &&
                           elapsed >= this->command_deadline_ms_;
    if (this->state_ == State::IDLE && this->pending_ && (ota_timeout || command_timeout)) {
      std::array<uint8_t, 260> padding;
      padding.fill(0xEF);
      this->state_ = State::PADDING;
      this->started_ms_ = now;
      this->command_probe_ = !ota_timeout;
      this->transport_.event(this->command_probe_ ? Event::COMMAND_TIMEOUT : Event::RECOVERY_STARTED);
      this->transport_.send_uart(padding.data(), padding.size());
    } else if (this->state_ == State::PADDING && elapsed >= 100) {
      this->probe_attempts_ = 1;
      this->send_probe_(now);
    } else if ((this->state_ == State::PING || this->state_ == State::DRAIN_PING ||
                this->state_ == State::REPLAY) && elapsed >= this->timeout_ms_) {
      if (this->command_probe_ && this->state_ == State::PING && this->pending_) {
        if (this->probe_attempts_ < 2) {
          this->probe_attempts_++;
          this->transport_.event(Event::COMMAND_PROBE_RETRY);
          this->send_probe_(now);
        } else if (!this->request_reset_(now)) {
          this->fail_();
        }
      } else {
        this->fail_();
      }
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

  static uint32_t command_deadline(uint8_t command0, uint8_t command1) {
    // Pings have their own watchdog. Startup/commissioning may intentionally block.
    if ((command0 == 0x21 && command1 == 0x01) ||
        (command0 == 0x25 && command1 == 0x40) ||
        (command0 == 0x2F && command1 == 0x05) ||
        (command0 == 0x26 && command1 == 0x00))
      return 0;
    if (command0 == 0x24 || command0 == 0x25 ||
        (command0 == 0x21 && (command1 == 0x02 || command1 == 0x05 || command1 == 0x08 ||
                             command1 == 0x13 || command1 == 0x1C || command1 == 0x32 || command1 == 0x33)))
      return 5000;
    // Writes and unfamiliar commands get a full minute before any probe is injected.
    return 60000;
  }

 protected:
  enum class State { IDLE, PADDING, PING, REPLAY, DRAIN_PING, FAILED };
  void fail_() {
    this->state_ = State::FAILED;
    this->transport_.event(Event::FAILED);
  }
  bool request_reset_(uint32_t now) {
    if (this->reset_attempted_ && now - this->last_reset_ms_ < 60000)
      return false;
    this->unanswered_pings_ = 0;
    this->reset_attempted_ = true;
    this->last_reset_ms_ = now;
    this->state_ = State::FAILED;
    this->transport_.event(Event::RADIO_UNRESPONSIVE);
    return true;
  }
  void send_probe_(uint32_t now) {
    static constexpr uint8_t PING[] = {0xFE, 0x00, 0x21, 0x01, 0x20};
    this->state_ = State::PING;
    this->started_ms_ = now;
    this->transport_.send_uart(PING, sizeof(PING));
  }

  Transport &transport_;
  Frame host_, radio_, saved_;
  uint32_t started_ms_{0};
  uint32_t last_radio_ms_{0}, last_host_ms_{0};
  uint32_t timeout_ms_{500};
  uint32_t last_ping_ms_{0}, last_reset_ms_{0};
  uint32_t command_deadline_ms_{0};
  uint8_t probe_attempts_{0};
  bool command_probe_{false};
  uint8_t unanswered_pings_{0};
  bool detect_unresponsive_{false}, reset_attempted_{false};
  uint8_t command0_{0}, command1_{0};
  bool pending_{false};
  State state_{State::IDLE};
};

}  // namespace esphome::znp_proxy
