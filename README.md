# ZNP OTA recovery proxy for ESPHome

ESPHome UART-to-TCP bridge for TI ZNP radios, with recovery for stalled Zigbee OTA
block responses. Uses native ESPHome UART/socket APIs; when coming from stock firmware or normal UART stream server Home Assistant needs no changes.

Tested on SLZB-06P7. The standard SLZB-06 (CC2652P running ZNP) is expected to work
with its 115200-baud radio firmware; it uses the same UART pins (RX GPIO5, TX GPIO17).
This recovery protocol does not support Silicon Labs radios or Thread firmware.

## Setup

Requires ESPHome 2026.10.2+ on an ESP32 with a dedicated 115200 8N1 UART.
Set that UART’s RX and TX buffers to at least `512B`.

Here is a complete ESPHome configuration for the SMLight SLZB-06 and SLZB-06P7 utilizing this component:

```yaml
# Customized version of https://github.com/smlight-dev/slzb-06-fw-esphome/blob/main/configs/zb-bt.yaml

esphome:
  friendly_name: SLZB-06P7
  name: slzb-06p7
  project:
    name: SMLIGHT.SLZB-06P7
    version: 1.51-custom
  on_boot:
    priority: 600
    then:
      - switch.turn_on: zRST_gpio
      - delay: 20ms
      - switch.turn_off: zRST_gpio

esp32:
  variant: ESP32
  flash_size: 16MB
  framework:
    type: esp-idf
    
external_components:
  - source: github://inventor7777/ESPHome-ZNP-Proxy
    components: [znp_proxy]

# UART settings
uart:
  - id: uart_bus
    rx_pin:
      number: GPIO5
      ignore_strapping_warning: true
    tx_pin:
      number: GPIO17
      # allow_other_uses: true  # Re-enable if using the TX-pin monitor.
    baud_rate: 115200
    rx_buffer_size: 512B
    tx_buffer_size: 512B
    debug:
      direction: BOTH
      dummy_receiver: false
      after:
        bytes: 64
        timeout: 5ms
      sequence:
        - lambda: |-
            static uint32_t tx_offset = 0;
            static uint32_t rx_offset = 0;
            uint32_t &offset = direction == uart::UART_DIRECTION_TX ? tx_offset : rx_offset;
            ESP_LOGD("uart_trace", "%s offset=%lu len=%u %s",
                     direction == uart::UART_DIRECTION_TX ? "TX" : "RX",
                     static_cast<unsigned long>(offset),
                     static_cast<unsigned>(bytes.size()),
                     format_hex_pretty(bytes).c_str());
            offset += bytes.size();

#   # Independently receive the ESP32 TX pin through the spare hardware UART.
#   - id: uart_tx_monitor
#     rx_pin:
#       number: GPIO17
#       allow_other_uses: true
#     baud_rate: 115200
#     rx_buffer_size: 1024B
#     debug:
#       direction: RX
#       dummy_receiver: true
#       after:
#         bytes: 64
#         timeout: 5ms
#       sequence:
#         - lambda: |-
#             static uint32_t offset = 0;
#             ESP_LOGD("uart_wire", "TXPIN offset=%lu len=%u %s",
#                      static_cast<unsigned long>(offset),
#                      static_cast<unsigned>(bytes.size()),
#                      format_hex_pretty(bytes).c_str());
#             offset += bytes.size();

# UART stream server
znp_proxy:
  id: streamserver
  uart_id: uart_bus
  port: 6638
  response_timeout: 500ms

# Log UART statistics to find issues
interval:
  - interval: 10s
    then:
      - lambda: "const auto port = static_cast<uart_port_t>(id(uart_bus).get_hw_serial_number());\nsize_t tx_free = 0;\nconst esp_err_t free_result = uart_get_tx_buffer_free_size(port, &tx_free);\nconst esp_err_t done_result = uart_wait_tx_done(port, 0);\nESP_LOGD(\"uart_driver\", \"port=%u tx_done=%s tx_free=%u free_result=%s rx_pending=%u\",\n         static_cast<unsigned>(port), esp_err_to_name(done_result),\n         static_cast<unsigned>(tx_free), esp_err_to_name(free_result),\n         static_cast<unsigned>(id(uart_bus).available()));"

# mDNS service settings
mdns:
  services:
    - service: "_slzb-06"
      protocol: "_tcp"
      port: 6638
      txt:
        version: 1.0
        name: SMLIGHT SLZB-06
        radio_type: znp
        baud_rate: 115200
        data_flow_control: software

# Physical button
binary_sensor:
  - platform: gpio
    name: "Button"
    id: gpio35btn1
    pin:
      number: GPIO35
      mode:
        input: true
      inverted: yes
    on_press:
      then:
        - switch.toggle: usbmodeswitch
  - platform: znp_proxy
    znp_proxy_id: streamserver
    connected:
      name: Serial

# Switches
switch:
## Zigbee Radio Reset switch
  - platform: gpio
    pin: 33
    id: zRST_gpio
    inverted: yes
    restore_mode: ALWAYS_OFF
## Zigbee Radio Flash Mode switch      
  - platform: gpio
    pin: 32
    id: zBSL_gpio
    inverted: yes
    restore_mode: ALWAYS_OFF

## Mode LAN/USB switch
  - platform: gpio
    name: "USB"
    id: usbmodeswitch
    pin: GPIO4
    restore_mode: RESTORE_DEFAULT_OFF
    entity_category: config
    on_turn_on:
      - light.turn_on: gpio12LED1
    on_turn_off:
      - light.turn_off: gpio12LED1

# LEDs
output:
  - platform: ledc
    id: blue_led_pwm
    pin:
      number: GPIO12
      ignore_strapping_warning: true

  - platform: ledc
    id: yellow_led_pwm
    pin: 
      number: GPIO14

light:
  - platform: monochromatic
    name: "Blue LED"
    id: gpio12LED1
    output: blue_led_pwm
    restore_mode: ALWAYS_OFF
    default_transition_length: 250ms
    icon: "mdi:led-on"

  - platform: monochromatic
    name: "Yellow LED"
    id: gpio14LED2
    output: yellow_led_pwm
    restore_mode: ALWAYS_ON
    default_transition_length: 250ms
    icon: "mdi:led-on"

# Buttons
button:
  - platform: template
    name: "Enter Radio Bootloader"
    icon: mdi:chip
    entity_category: config
    on_press:
      - switch.turn_on: zBSL_gpio
      - delay: 1s
      - switch.turn_on: zRST_gpio
      - delay: 1s
      - switch.turn_off: zRST_gpio
      - logger.log: "Wait 1 seconds for Zigbee chip to be ready"
      - delay: 1s
      - switch.turn_off: zBSL_gpio
      - logger.log: "Update Zigbee with your Z-Stack firmware now!"

  - platform: template
    name: "Restart Radio"
    icon: mdi:restart
    entity_category: config
    on_press:
      - switch.turn_off: zBSL_gpio
      - switch.turn_on: zRST_gpio
      - delay: 15ms
      - switch.turn_off: zRST_gpio

logger:
  level: DEBUG

api:
  reboot_timeout: 0s # Allow Zigbee use without HA API connected
  encryption:
    key: "REPLACEME"
    
ota:
  - platform: esphome
    encryption: 
    allow_partition_access: true
    id: esphome_ota

network:
  enable_high_performance: true # Better performance for Zigbee over TCP

# Ethernet connection
ethernet:
  type: LAN8720
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk:
    pin:
      number: GPIO0
      ignore_strapping_warning: true
    mode: CLK_EXT_IN
  phy_addr: 1
  power_pin: GPIO16
```

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

## Notes
- Tested and works flawlessly on my SMLight SLZB-06p7 running my customized ESPHome firmware.
- Code written by GPT-6.1 Sol after extensive analysis of the issue and logs from both ends.
