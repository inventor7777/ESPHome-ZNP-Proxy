#include "znp_proxy.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>
#include <cinttypes>

namespace esphome::znp_proxy {

static const char *const TAG = "znp_proxy";

void ZnpProxy::setup() {
  this->link_.begin(TAG);
  this->listener_.begin(TAG);
#ifdef USE_BINARY_SENSOR
  if (this->connected_ != nullptr)
    this->connected_->publish_state(false);
#endif
}

void ZnpProxy::dump_config() {
  ESP_LOGCONFIG(TAG, "ZNP OTA recovery proxy:\n  Listen port: %u\n  UART: 115200 8N1", this->link_.port());
  this->listener_.dump_config();
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Connected", this->connected_);
#endif
}

void ZnpProxy::on_shutdown() {
  this->link_.close();
  this->listener_.close();
}

void ZnpProxy::send_uart(const uint8_t *data, size_t size) {
  if (this->uart_tx_size_ != 0 || size > this->uart_tx_.size()) {
    this->transport_failed_ = true;
    return;
  }
  std::memcpy(this->uart_tx_.data(), data, size);
  this->uart_tx_size_ = size;
  this->uart_tx_offset_ = 0;
}

void ZnpProxy::send_host(const uint8_t *data, size_t size) {
  if (this->link_.queue(data, size) != size)
    this->transport_failed_ = true;
}

void ZnpProxy::event(Event event) {
  switch (event) {
    case Event::RECOVERY_STARTED:
      ESP_LOGW(TAG, "OTA SRSP missing: txn=%u offset=%" PRIu32 "; trying UART resync (no radio reset)",
               this->protocol_.transaction(), this->protocol_.block_offset());
      break;
    case Event::REPLAY_STARTED:
      ESP_LOGW(TAG, "Internal ping answered; replaying the OTA block once");
      break;
    case Event::RECOVERED:
      ESP_LOGI(TAG, "OTA block SRSP received after recovery");
      break;
    case Event::LATE_RESPONSE:
      ESP_LOGI(TAG, "Original SRSP arrived late; replay cancelled");
      break;
    case Event::FAILED:
      ESP_LOGE(TAG, "Recovery failed or response ownership is ambiguous; disconnecting");
      break;
  }
}

void ZnpProxy::sync_link_() {
  this->link_was_up_ = this->link_.connected();
  this->protocol_.reset();
  this->uart_tx_size_ = 0;
  this->socket_pending_ = false;
  this->transport_failed_ = false;
  if (this->link_was_up_) {
    // Discard only the snapshot buffered before this connection.
    size_t left = this->available();
    uint8_t byte;
    while (left-- != 0 && this->read_byte(&byte)) {}
  }
#ifdef USE_BINARY_SENSOR
  if (this->connected_ != nullptr)
    this->connected_->publish_state(this->link_was_up_);
#endif
}

void ZnpProxy::pump_uart_() {
  if (this->uart_tx_size_ == 0)
    return;
  size_t room = this->parent_->available_for_write();
  // shortcut: platforms without capacity reporting send at most 16 bytes per loop.
  if (room == SIZE_MAX)
    room = 16;
  size_t count = std::min({room, this->uart_tx_size_ - this->uart_tx_offset_, size_t(128)});
  if (count == 0)
    return;
  this->write_array(this->uart_tx_.data() + this->uart_tx_offset_, count);
  this->uart_tx_offset_ += count;
  if (this->uart_tx_offset_ == this->uart_tx_size_)
    this->uart_tx_size_ = 0;
}

void ZnpProxy::loop() {
  this->listener_.poll(this->link_, !this->link_was_up_);
  if (this->link_.connected() != this->link_was_up_)
    this->sync_link_();
  if (!this->link_was_up_)
    return;
  const uint32_t now = App.get_loop_component_start_time();
  this->link_.flush_tx();
  // Reserve room for a whole frame before consuming any bytes from the UART.
  size_t budget = std::min<size_t>(this->available(), 512);
  uint8_t byte;
  while (budget-- != 0 && this->link_.tx_free() >= 260 && this->read_byte(&byte)) {
    this->protocol_.radio_byte(byte, now);
    if (this->protocol_.failed() || this->transport_failed_)
      break;
  }
  this->pump_uart_();
  // Drain queued UART work before advancing timers or admitting more host data.
  if (this->uart_tx_size_ == 0 && this->link_.tx_free() >= 260) {
    this->protocol_.tick(now);
    if (!this->protocol_.paused() && !this->transport_failed_ &&
        (this->socket_pending_ || this->link_.ready())) {
      uint8_t buffer[128];
      ssize_t count = this->link_.read(buffer, sizeof(buffer));
      this->socket_pending_ = count == sizeof(buffer);
      if (count > 0)
        this->protocol_.host_data(buffer, static_cast<size_t>(count), now);
    }
  }
  this->pump_uart_();
  this->link_.flush_tx();
  if (this->protocol_.failed() || this->transport_failed_)
    this->link_.close();
}

}  // namespace esphome::znp_proxy
