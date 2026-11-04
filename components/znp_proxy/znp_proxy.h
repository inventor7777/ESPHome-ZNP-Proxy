#pragma once

#include "protocol.h"
#include "esphome/components/socket/tcp_client_link.h"
#include "esphome/components/socket/tcp_listener.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome::znp_proxy {

class ZnpProxy : public Component, public uart::UARTDevice, public Transport {
 public:
  ZnpProxy() : protocol_(*this) {}
  void set_port(uint16_t port) { this->link_.set_port(port); }
  void set_response_timeout(uint32_t ms) { this->protocol_.set_response_timeout(ms); }
#ifdef USE_SOCKET_IPV4_ALLOW
  void set_allow(const socket::Ipv4AllowEntry *entries, size_t count) { this->listener_.set_allow(entries, count); }
#endif
#ifdef USE_BINARY_SENSOR
  void set_connected_sensor(binary_sensor::BinarySensor *sensor) { this->connected_ = sensor; }
#endif
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void send_uart(const uint8_t *data, size_t size) override;
  void send_host(const uint8_t *data, size_t size) override;
  void event(Event event) override;

 protected:
  void pump_uart_();
  void sync_link_();
  socket::TcpClientLink link_;
  socket::TcpListener listener_;
  Protocol protocol_;
  std::array<uint8_t, 260> uart_tx_{};
  size_t uart_tx_size_{0}, uart_tx_offset_{0};
  bool link_was_up_{false};
  bool socket_pending_{false};
  bool transport_failed_{false};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *connected_{nullptr};
#endif
};

}  // namespace esphome::znp_proxy
