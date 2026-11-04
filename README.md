# ZNP OTA recovery proxy

ESPHome UART-to-TCP bridge for TI ZNP radios, with recovery for stalled Zigbee OTA
block responses. Uses native ESPHome UART/socket APIs; Home Assistant needs no changes.

Tested on SLZB-06P7. The standard SLZB-06 (CC2652P running ZNP) is expected to work
with its 115200-baud radio firmware; it uses the same UART pins (RX GPIO5, TX GPIO17).
This recovery protocol does not support Silicon Labs radios or Thread firmware.

## Setup

Requires ESPHome 2026.10.2+, ESP32, networking, and a dedicated 115200 8N1 UART.
Set that UART’s RX and TX buffers to at least `512B`.

Replace `uart_tcp` and its connected sensor with:

```yaml
external_components:
  - source:
      type: local
      path: Components/znp_proxy/components
    components: [znp_proxy]

znp_proxy:
  id: streamServer
  uart_id: uart_bus
  port: 6638
  response_timeout: 500ms  # Optional; allowed range: 250ms–1000ms
  # allowed_ips: [192.168.50.3]  # Optional IPv4 client restriction

binary_sensor:
  - platform: znp_proxy
    znp_proxy_id: streamServer
    connected:
      name: Serial
```

Keep your existing UART, Ethernet, mDNS, and radio controls. Remove the old bridge
and manual UART resync button. Keep the GPIO4 mode switch in LAN mode.

## Behavior

If an eligible successful OTA block response gets no synchronous radio response,
the proxy pauses new host input, sends 260 resync bytes, and checks a private ping.
If the ping answers, it replays that block once and forwards the radio’s real
responses. A replay's DataConfirm may arrive before its synchronous response;
the proxy forwards it and keeps waiting. Internal ping replies stay hidden from HA;
no success is fabricated.

Warnings mark recovery attempts; an INFO message marks receipt of the block’s
synchronous response. Failure logs an ERROR and closes TCP so HA can reconnect.
The component never resets the radio. While disconnected, UART bytes remain
buffered and are discarded on the next connection.

## Limits and checks

Recovery and subsequent block progress have been observed on hardware, but a full
OTA completion has not yet been confirmed. A radio that remains silent can still
stop the transfer. Response matching assumes the radio answers older synchronous
requests before the private ping; those responses have no transaction IDs.

This is a ZNP bridge, not a raw radio-flashing transport. Use USB for radio firmware
flashing. TCP has no authentication; use a trusted network or `allowed_ips`.

Tests: `sh tests/run.sh` runs protocol and transport checks with address/undefined
behavior sanitizers. Transport checks exercise the real adapter against fake UART
and TCP interfaces: partial writes, backpressure, and disconnect/reconnect cleanup.
`python tests/test_config.py` checks configuration in an ESPHome Python environment.
