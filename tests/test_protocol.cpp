#include "../components/znp_proxy/protocol.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace esphome::znp_proxy;
using Bytes = std::vector<uint8_t>;

Bytes frame(uint8_t cmd0, uint8_t cmd1, Bytes payload = {}) {
  Bytes result{0xFE, static_cast<uint8_t>(payload.size()), cmd0, cmd1};
  result.insert(result.end(), payload.begin(), payload.end());
  uint8_t checksum = 0;
  for (size_t i = 1; i < result.size(); i++)
    checksum ^= result[i];
  result.push_back(checksum);
  return result;
}

Bytes ota() {
  // Same AF/ZCL layout as the failed block at offset 28900, without firmware data.
  Bytes payload(87, 0);
  payload[0] = 2;
  payload[1] = 0x0E;
  payload[2] = 0xE8;
  payload[9] = 1;
  payload[12] = 1;
  payload[13] = 0x19;
  payload[15] = 108;
  payload[16] = 0x30;
  payload[18] = 67;
  payload[20] = 0x19;
  payload[21] = 108;
  payload[22] = 5;
  payload[32] = 0xE4;
  payload[33] = 0x70;
  payload[36] = 50;
  return frame(0x24, 0x02, payload);
}

const Bytes PING = frame(0x21, 0x01);
const Bytes PONG = frame(0x61, 0x01, {0x59, 0x06});
const Bytes SRSP = frame(0x64, 0x02, {0});
const Bytes CONFIRM = frame(0x44, 0x80, {0, 1, 108});
const Bytes INCOMING = frame(0x44, 0x81, Bytes(25, 0));

struct Harness : Transport {
  Protocol protocol{*this};
  std::vector<Bytes> uart, host;
  std::vector<Event> events;
  void send_uart(const uint8_t *data, size_t size) override { uart.emplace_back(data, data + size); }
  void send_host(const uint8_t *data, size_t size) override { host.emplace_back(data, data + size); }
  void event(Event event) override { events.push_back(event); }
  void tx(const Bytes &data, uint32_t now = 0) { protocol.host_data(data.data(), data.size(), now); }
  void rx(const Bytes &data, uint32_t now) {
    for (auto byte : data)
      protocol.radio_byte(byte, now);
  }
  void start(uint32_t base = 0) {
    tx(ota(), base);
    protocol.tick(base + 499);
    assert(uart.size() == 1);
    protocol.tick(base + 500);
    assert(protocol.paused());
    assert(uart.back() == Bytes(260, 0xEF));
    protocol.tick(base + 599);
    assert(uart.size() == 2);
    protocol.tick(base + 600);
    assert(uart.back() == PING);
  }
};

int main() {
  {
    Harness h;
    const auto block = ota();
    // TCP segmentation must not change the tracked frame.
    for (auto byte : block)
      h.tx({byte});
    h.rx(SRSP, 25);
    h.rx(CONFIRM, 30);
    h.protocol.tick(10000);
    assert(h.uart.size() == block.size());
    assert(h.host == std::vector<Bytes>({SRSP, CONFIRM}));
    assert(h.events.empty());
  }
  {
    Harness h;
    h.start();
    h.rx(INCOMING, 610);
    h.rx(PONG, 620);
    assert(h.uart.back() == ota());
    assert(h.host == std::vector<Bytes>({INCOMING}));
    h.rx(SRSP, 640);
    h.rx(CONFIRM, 660);
    assert(!h.protocol.paused());
    assert(h.host == std::vector<Bytes>({INCOMING, SRSP, CONFIRM}));
    h.protocol.tick(2000);
    assert(h.uart.size() == 4);
    assert(h.events.back() == Event::RECOVERED);
  }
  {
    Harness h;
    h.start();
    h.rx(SRSP, 610);  // Original late reply while the internal ping is in flight.
    assert(h.protocol.paused());
    h.rx(PONG, 620);
    assert(!h.protocol.paused());
    assert(h.uart.size() == 3);  // No replay and no internal ping reply leaked.
    assert(h.host == std::vector<Bytes>({SRSP}));
    h.tx(PING, 630);
    h.rx(PONG, 640);
    assert(h.host.back() == PONG);  // A later host ping works normally.
  }
  {
    Harness h;
    h.tx(ota());
    h.protocol.tick(500);
    h.rx(SRSP, 550);  // Original reply before an internal ping was sent.
    h.protocol.tick(600);
    assert(!h.protocol.paused());
    assert(h.uart.size() == 2);
  }
  {
    Harness h;
    h.start();
    h.protocol.tick(1100);
    assert(h.protocol.failed());
    assert(h.uart.size() == 3);
    assert(h.host.empty());
  }
  {
    Harness h;
    h.start();
    h.rx(PONG, 620);
    h.protocol.tick(1120);
    assert(h.protocol.failed());
    assert(h.uart.size() == 4);  // Never a second replay.
    assert(h.host.empty());     // Never fabricate an SRSP.
  }
  {
    Harness h;
    h.start();
    h.rx(SRSP, 650);
    h.protocol.tick(1100);
    assert(h.protocol.failed());  // Must not hand an unconsumed internal ping to HA.
  }
  {
    Harness h;
    h.tx(ota());
    h.rx(CONFIRM, 20);
    h.protocol.tick(500);
    assert(h.uart.size() == 1);  // Already executed: don't replay.
    Harness recovering;
    recovering.start();
    recovering.rx(CONFIRM, 610);
    assert(recovering.protocol.failed());
  }
  {
    for (const auto &request : {PING, frame(0x21, 0x33, {1, 2}), frame(0x24, 0x01, {1, 2})}) {
      Harness h;
      h.tx(request);
      h.protocol.tick(10000);
      assert(h.uart.size() == 1);
      assert(!h.protocol.paused());
    }
    // Non-OTA cluster, abort response, wrong length, manufacturer-specific header.
    for (size_t offset : {size_t(17), size_t(27), size_t(22), size_t(24), size_t(40)}) {
      auto bad = ota();
      bad[offset] ^= 1;
      bad.back() ^= 1;
      Harness h;
      h.tx(bad);
      h.protocol.tick(500);
      assert(h.uart.size() == 1);
    }
  }
  {
    Harness h;
    auto corrupted = ota();
    corrupted.back() ^= 1;
    h.tx(corrupted);
    h.protocol.tick(500);
    assert(h.uart.size() == 1);
    // A bad received FCS is dropped; the following frame still arrives.
    auto bad_rsp = SRSP;
    bad_rsp.back() ^= 1;
    h.rx(bad_rsp, 510);
    h.rx(INCOMING, 520);
    assert(h.host == std::vector<Bytes>({INCOMING}));
    h.rx({0xFE, 0xFF, 0x64}, 530);
    h.protocol.tick(630);  // Assembly timeout clears the incomplete frame.
    h.rx(PONG, 640);
    assert(h.host.back() == PONG);
  }
  {
    Harness h;
    h.start();
    h.rx(frame(0x41, 0x80, {1, 2, 3}), 610);
    assert(h.protocol.failed());
    h.protocol.reset();  // A new TCP connection must not inherit a replay.
    h.protocol.tick(5000);
    assert(!h.protocol.paused());
    assert(h.uart.size() == 3);
  }
  {
    Harness h;
    h.tx(ota());
    h.tx(PING, 1);  // Unexpected concurrent host command disables recovery.
    h.protocol.tick(500);
    assert(h.uart.size() == 2);
    assert(!h.protocol.paused());
  }
  {
    Harness h;
    h.start(0xFFFFFF00);  // Timers work across uint32_t wraparound.
    h.rx(PONG, uint32_t(0xFFFFFF00 + 620));
    h.rx(SRSP, uint32_t(0xFFFFFF00 + 650));
    assert(!h.protocol.paused());
    assert(h.events.back() == Event::RECOVERED);
  }
  {
    Harness h;
    Frame radio_parser;
    auto consume_at_radio = [&](const Bytes &bytes) {
      size_t received = 0;
      for (auto byte : bytes) {
        if (radio_parser.feed(byte)) {
          received++;
          radio_parser.clear();
        }
      }
      return received;
    };
    h.tx(ota());
    auto damaged = h.uart.back();
    damaged.erase(damaged.begin() + 41);  // Model one byte lost at the radio.
    assert(consume_at_radio(damaged) == 0);
    h.protocol.tick(500);
    assert(consume_at_radio(h.uart.back()) == 0);  // Padding finishes the bad frame.
    h.protocol.tick(600);
    assert(consume_at_radio(h.uart.back()) == 1);  // Ping is a new, valid frame.
    h.rx(PONG, 620);
    assert(consume_at_radio(h.uart.back()) == 1);  // Original OTA frame is intact.
    h.rx(SRSP, 640);
    assert(h.events.back() == Event::RECOVERED);
  }
  {
    Harness h;
    h.start();
    h.rx(PONG, 620);
    h.rx(CONFIRM, 630);  // The replay's callback can precede its synchronous reply.
    assert(h.protocol.paused());
    assert(!h.protocol.failed());
    assert(h.host == std::vector<Bytes>({CONFIRM}));
    h.rx(SRSP, 650);
    assert(!h.protocol.paused());
    assert(h.events.back() == Event::RECOVERED);
    assert(h.host == std::vector<Bytes>({CONFIRM, SRSP}));
    assert(h.uart.size() == 4);
  }
  {
    Harness h;
    h.start();
    h.rx(PONG, 620);
    h.rx(CONFIRM, 630);
    h.protocol.tick(1120);  // A callback must not fabricate a missing SRSP.
    assert(h.protocol.failed());
    assert(h.host == std::vector<Bytes>({CONFIRM}));
    assert(h.uart.size() == 4);
  }
  {
    Frame parser;
    auto maximum = frame(0x44, 0x81, Bytes(255, 0xFE));
    for (size_t i = 0; i < maximum.size(); i++)
      assert(parser.feed(maximum[i]) == (i + 1 == maximum.size()));
    assert(parser.size == 260);
  }
  {
    // Worst partial frame: only FE FF received; 258 bytes still needed.
    Frame radio_parser;
    assert(!radio_parser.feed(0xFE));
    assert(!radio_parser.feed(0xFF));
    Harness h;
    h.start();
    for (auto byte : h.uart[1]) {
      if (radio_parser.feed(byte))
        radio_parser.clear();
    }
    assert(radio_parser.size == 0);
    size_t received = 0;
    for (auto byte : h.uart[2]) {
      if (radio_parser.feed(byte)) {
        received++;
        radio_parser.clear();
      }
    }
    assert(received == 1);  // The recovery ping survives the padding boundary.
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(PING, 0);
    h.tx(PING, 1000);
    h.protocol.tick(1499);
    assert(h.events.empty());
    h.protocol.tick(1500);
    assert(h.events == std::vector<Event>({Event::RADIO_UNRESPONSIVE}));
    h.protocol.reset();  // Reconnecting must not erase the reset cooldown.
    h.tx(PING, 2000);
    h.tx(PING, 3000);
    h.protocol.tick(3500);
    assert(h.events.size() == 1);
    h.protocol.reset();
    h.tx(PING, 60000);
    h.tx(PING, 61000);
    h.protocol.tick(61499);
    assert(h.events.size() == 1);
    h.protocol.tick(61500);
    assert(h.events.size() == 2);
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    for (uint32_t now : {0, 3000, 4000}) {
      h.tx(PING, now);
      h.rx(PONG, now + 20);
    }
    h.protocol.tick(10000);
    assert(h.events.empty());
    assert(h.host.size() == 3);  // Host ping replies are never swallowed.
    h.tx(PING, 11000);
    h.protocol.tick(20000);
    assert(h.events.empty());  // One unanswered probe is insufficient.
  }
  {
    Harness h;
    h.tx(PING, 0);
    h.tx(PING, 1000);
    h.protocol.tick(1500);
    assert(h.events.empty());  // No automation means no automatic resets.
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(PING, 0);
    h.tx(PING, 1000);
    h.rx(frame(0x41, 0x80, {1, 2, 3}), 1200);
    h.protocol.tick(1500);
    assert(h.events.empty());  // A manual reset cancels the old failure streak.
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    auto command = frame(0x21, 0x02);  // SYS.Version is an ordinary, non-OTA request.
    h.tx(command, 0);
    h.rx(INCOMING, 4900);  // Sensor reports do not answer the pending SREQ.
    h.protocol.tick(4999);
    assert(h.uart.size() == 1);
    h.protocol.tick(5000);
    assert(h.protocol.paused());
    assert(h.uart.back() == Bytes(260, 0xEF));
    assert(h.events.back() == Event::COMMAND_TIMEOUT);
    h.protocol.tick(5100);
    assert(h.uart.back() == PING);
    h.rx(PONG, 5150);
    assert(h.protocol.failed());  // Let the host reconnect; do not repeat Version.
    assert(h.events.back() == Event::COMMAND_RESPONSIVE);
    assert(h.host == std::vector<Bytes>({INCOMING}));
    assert(h.uart == std::vector<Bytes>({command, Bytes(260, 0xEF), PING}));
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x24, 0x01, {1, 2}), 0);
    h.protocol.tick(5000);
    h.protocol.tick(5100);
    h.protocol.tick(5599);
    assert(h.uart.size() == 3);
    h.protocol.tick(5600);
    assert(h.uart.back() == PING);
    assert(h.events.back() == Event::COMMAND_PROBE_RETRY);
    h.protocol.tick(6099);
    assert(!h.protocol.failed());
    h.protocol.tick(6100);
    assert(h.protocol.failed());
    assert(h.events.back() == Event::RADIO_UNRESPONSIVE);
    assert(h.uart.size() == 4);  // Two probes, zero ordinary-command replays.
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x21, 0x02), 0);
    h.protocol.tick(5000);
    h.rx(frame(0x61, 0x02, Bytes(9, 0)), 5050);  // Late response before the probe.
    h.protocol.tick(5100);
    assert(!h.protocol.paused());
    assert(h.uart.size() == 2);
    assert(h.events.back() == Event::LATE_RESPONSE);
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x21, 0x02), 0);
    h.protocol.tick(5000);
    h.protocol.tick(5100);
    h.protocol.tick(5600);  // A second private probe is now outstanding.
    h.rx(frame(0x61, 0x02, Bytes(9, 0)), 5650);
    h.protocol.tick(6100);
    assert(h.protocol.failed());  // Cancel reset; close rather than leaking a late pong.
    assert(std::find(h.events.begin(), h.events.end(), Event::RADIO_UNRESPONSIVE) == h.events.end());
    assert(h.host.size() == 1);
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x21, 0x02), 0);
    h.protocol.tick(5000);
    h.protocol.tick(5100);
    h.protocol.tick(5600);
    h.rx(PONG, 5650);  // Either probe's answer proves the command path responds.
    assert(h.events.back() == Event::COMMAND_RESPONSIVE);
    assert(h.host.empty());
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x21, 0x02), 0);
    h.rx(frame(0x60, 0x00, {1, 0x21, 0x02}), 100);  // RPC command error is a real reply.
    h.protocol.tick(100000);
    assert(h.events.empty());
    assert(h.uart.size() == 1);
    assert(h.host.size() == 1);
  }
  {
    for (auto command : {frame(0x25, 0x40, {0, 0}), frame(0x2F, 0x05, {1}), frame(0x26, 0x00)}) {
      Harness h;
      h.protocol.set_unresponsive_detection(true);
      h.tx(command, 0);
      h.protocol.tick(120000);
      assert(h.events.empty());  // Startup and commissioning are explicitly excluded.
    }
    for (auto command : {frame(0x21, 0x34, {1, 2}), frame(0x21, 0x30, {1, 2}), frame(0x28, 0x55)}) {
      Harness h;
      h.protocol.set_unresponsive_detection(true);
      h.tx(command, 0);
      h.protocol.tick(59999);
      assert(h.events.empty());  // Writes and unknown commands get a minute.
      h.protocol.tick(60000);
      assert(h.events.back() == Event::COMMAND_TIMEOUT);
    }
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x21, 0x02), 0);
    h.rx(frame(0x61, 0x02, Bytes(9, 0)), 100);
    h.protocol.tick(100000);
    assert(h.events.empty());  // A quiet, healthy network never triggers probing.
    h.tx(frame(0x21, 0x02), 101000);
    h.tx(frame(0x21, 0x08, {1, 2}), 101001);
    h.protocol.tick(200000);
    assert(h.events.empty());  // Multiple pending SREQs cannot be safely attributed.
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x21, 0x02), 0);
    h.protocol.tick(5000);
    h.protocol.reset();
    h.protocol.tick(100000);
    assert(h.events.size() == 1);  // No probes survive a disconnect/reset boundary.
  }
  {
    Harness h;
    h.tx(frame(0x21, 0x02), 0);
    h.protocol.tick(100000);
    assert(h.events.empty());  // Remains opt-in through the existing reset automation.
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x21, 0x02), 0xFFFFFF00);
    h.protocol.tick(uint32_t(0xFFFFFF00 + 5000));
    h.protocol.tick(uint32_t(0xFFFFFF00 + 5100));
    h.protocol.tick(uint32_t(0xFFFFFF00 + 5600));
    h.protocol.tick(uint32_t(0xFFFFFF00 + 6100));
    assert(h.events.back() == Event::RADIO_UNRESPONSIVE);
  }
  {
    Harness h;
    h.protocol.set_unresponsive_detection(true);
    h.tx(frame(0x21, 0x02), 0);
    h.protocol.tick(5000);
    h.protocol.tick(5100);
    h.rx(frame(0x60, 0x00, {1, 0x21, 0x01}), 5150);
    assert(h.events.back() == Event::COMMAND_RESPONSIVE);  // Even a probe error proves execution.
    assert(h.host.empty());  // Its RPC error belongs to the private probe, not the host command.
    assert(h.protocol.failed());
  }
  std::puts("All ZNP protocol checks passed");
  return 0;
}
