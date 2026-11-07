#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <sys/types.h>
#include <vector>

namespace esphome {
template<typename... Args> class Trigger {
 public:
  size_t calls{0};
  void trigger(Args...) { this->calls++; }
};
namespace setup_priority {
inline constexpr float AFTER_WIFI = 200;
}
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual void on_shutdown() {}
  virtual float get_setup_priority() const { return 600; }
};
inline struct Application {
  uint32_t now{0};
  uint32_t get_loop_component_start_time() const { return this->now; }
} App;

namespace uart {
class UARTComponent {
 public:
  size_t room{512};
  std::deque<uint8_t> rx;
  std::vector<uint8_t> written;
  size_t available_for_write() const { return this->room; }
};
class UARTDevice {
 public:
  size_t available() const { return this->parent_->rx.size(); }
  bool read_byte(uint8_t *byte) {
    if (this->parent_->rx.empty())
      return false;
    *byte = this->parent_->rx.front();
    this->parent_->rx.pop_front();
    return true;
  }
  void write_array(const uint8_t *data, size_t size) {
    assert(size <= this->parent_->room);
    if (this->parent_->room != SIZE_MAX)
      this->parent_->room -= size;
    this->parent_->written.insert(this->parent_->written.end(), data, data + size);
  }
 protected:
  UARTComponent *parent_{nullptr};
};
}  // namespace uart

namespace socket {
class TcpClientLink {
 public:
  bool up{false};
  size_t flush_limit{SIZE_MAX};
  size_t reads{0};
  std::deque<uint8_t> input;
  std::vector<uint8_t> pending, received;
  void begin(const char *) {}
  void set_port(uint16_t port) { this->port_ = port; }
  uint16_t port() const { return this->port_; }
  bool connected() const { return this->up; }
  bool ready() const { return !this->input.empty(); }
  size_t tx_free() const { return this->up ? 1024 - this->pending.size() : 0; }
  ssize_t read(uint8_t *data, size_t size) {
    this->reads++;
    size_t count = std::min(size, this->input.size());
    for (size_t i = 0; i < count; i++) {
      data[i] = this->input.front();
      this->input.pop_front();
    }
    return count;
  }
  size_t queue(const uint8_t *data, size_t size) {
    size_t count = std::min(size, this->tx_free());
    this->pending.insert(this->pending.end(), data, data + count);
    return count;
  }
  bool flush_tx() {
    size_t count = std::min(this->pending.size(), this->flush_limit);
    this->received.insert(this->received.end(), this->pending.begin(), this->pending.begin() + count);
    this->pending.erase(this->pending.begin(), this->pending.begin() + count);
    return this->pending.empty();
  }
  void close() {
    this->up = false;
    this->pending.clear();
    this->input.clear();
  }
 protected:
  uint16_t port_{6638};
};
class TcpListener {
 public:
  void begin(const char *) {}
  void poll(TcpClientLink &, bool) {}
  void dump_config() const {}
  void close() {}
};
}  // namespace socket

template<typename... Args> void fake_log(Args...) {}
}  // namespace esphome

#define ESP_LOGCONFIG(...) ::esphome::fake_log(__VA_ARGS__)
#define ESP_LOGW(...) ::esphome::fake_log(__VA_ARGS__)
#define ESP_LOGI(...) ::esphome::fake_log(__VA_ARGS__)
#define ESP_LOGE(...) ::esphome::fake_log(__VA_ARGS__)
