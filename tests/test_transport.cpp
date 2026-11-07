// Compile the actual adapter against bounded, controllable UART/socket fakes.
#include "fakes.h"
#include "../components/znp_proxy/znp_proxy.cpp"
#include <cstdio>

using namespace esphome;
using namespace esphome::znp_proxy;
using Bytes = std::vector<uint8_t>;
const Bytes PING{0xFE, 0x00, 0x21, 0x01, 0x20};
const Bytes PONG{0xFE, 0x02, 0x61, 0x01, 0x59, 0x06, 0x3D};
const Bytes SRSP{0xFE, 0x01, 0x64, 0x02, 0x00, 0x67};

struct Harness : ZnpProxy {
  uart::UARTComponent uart;
  Harness() { this->parent_ = &this->uart; this->setup(); }
  socket::TcpClientLink &link() { return this->link_; }
  Protocol &protocol() { return this->protocol_; }
  size_t queued() const { return this->uart_tx_size_; }
  void connect() { this->link_.up = true; this->step(0); }
  void step(uint32_t now, size_t room = 512) {
    App.now = now;
    this->uart.room = room;
    this->loop();
  }
  void host(const Bytes &bytes) { this->link_.input.insert(this->link_.input.end(), bytes.begin(), bytes.end()); }
  void radio(const Bytes &bytes) { this->uart.rx.insert(this->uart.rx.end(), bytes.begin(), bytes.end()); }
};

Bytes ota() {
  Bytes bytes(43, 0);
  bytes[0] = 0xFE;
  bytes[1] = 38;
  bytes[2] = 0x24;
  bytes[3] = 0x02;
  bytes[4] = 2;
  bytes[16] = 1;
  bytes[17] = 0x19;
  bytes[19] = 108;
  bytes[22] = 18;
  bytes[24] = 0x19;
  bytes[26] = 5;
  bytes[40] = 1;
  for (size_t i = 1; i + 1 < bytes.size(); i++)
    bytes.back() ^= bytes[i];
  return bytes;
}

int main() {
  {
    Harness h;
    h.connect();
    Bytes input(300, 0xEF);
    h.host(input);
    h.step(16, 2);
    assert(h.uart.written.size() == 2);
    assert(h.link().input.size() == 172);
    h.step(32, 2);
    assert(h.uart.written.size() == 4);
    assert(h.link().reads == 1);  // Backpressure: don't overwrite a partial UART write.
    for (uint32_t now = 48; now <= 160; now += 16)
      h.step(now);
    assert(h.uart.written == input);
    assert(h.queued() == 0);
  }
  {
    Harness h;
    h.connect();
    h.link().flush_limit = 0;
    h.link().pending.assign(800, 0xEF);
    h.radio(SRSP);
    h.host(PING);
    h.step(16);
    assert(h.uart.rx.size() == SRSP.size());
    assert(h.link().reads == 0);  // Stop both directions when TCP has no frame room.
    h.link().flush_limit = 64;
    h.step(32);
    assert(h.uart.rx.empty());
    assert(h.uart.written == PING);
    for (uint32_t now = 48; now <= 256; now += 16)
      h.step(now);
    assert(h.link().pending.empty());
    assert(h.link().received.size() == 800 + SRSP.size());
    assert(std::equal(SRSP.begin(), SRSP.end(), h.link().received.end() - SRSP.size()));
  }
  {
    Harness h;
    h.radio(SRSP);
    h.step(0);
    assert(h.uart.rx.size() == SRSP.size());  // Disconnected bytes stay buffered.
    h.connect();
    assert(h.uart.rx.empty());  // Old replies are discarded at the connection boundary.
    assert(h.link().received.empty());
    h.radio(SRSP);
    h.step(16);
    assert(h.link().received == SRSP);  // New replies are forwarded.
  }
  {
    Harness h;
    h.connect();
    const auto request = ota();
    h.host(request);
    h.step(16);
    h.step(516);  // Start padding.
    h.step(532);  // Drain padding, including the four extra bytes.
    h.step(616);  // Private ping.
    h.radio(PONG);
    h.step(632, 0);  // Radio answered, but replay cannot enter the UART yet.
    assert(h.protocol().paused());
    assert(h.queued() == request.size());
    assert(h.link().received.empty());  // Internal ping reply is not leaked.
    const auto already_sent = h.uart.written;
    h.link().close();
    h.step(648);
    assert(h.queued() == 0);
    assert(!h.protocol().paused());
    h.connect();
    h.host(PING);
    h.step(664);
    Bytes expected = already_sent;
    expected.insert(expected.end(), PING.begin(), PING.end());
    assert(h.uart.written == expected);  // No replay from the previous connection.
  }
  {
    Harness h;
    h.connect();
    h.host(Bytes(40, 0xEF));
    h.step(16, SIZE_MAX);
    assert(h.uart.written.size() == 16);  // Capacity-unknown fallback is bounded.
    h.step(32, SIZE_MAX);
    assert(h.uart.written == Bytes(40, 0xEF));
  }
  {
    Harness h;
    h.set_unresponsive_detection(true);
    h.connect();
    for (uint32_t now : {0, 3000, 4000}) {
      h.host(PING);
      h.step(now);
    }
    h.step(4499);
    assert(h.link().connected());
    h.step(4500);
    assert(!h.link().connected());
    assert(h.get_radio_unresponsive_trigger()->calls == 1);
    assert(h.queued() == 0);
    assert(!h.protocol().paused());
    h.connect();
    for (uint32_t now : {5000, 6000, 7000}) {
      h.host(PING);
      h.step(now);
    }
    h.step(8500);
    assert(h.link().connected());
    assert(h.get_radio_unresponsive_trigger()->calls == 1);
  }
  std::puts("All ZNP transport checks passed");
}
