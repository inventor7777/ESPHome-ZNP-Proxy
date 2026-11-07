"""Run with the same Python environment as ESPHome: python tests/test_config.py."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "components"
BASE = """
esphome:
  name: znp-proxy-test
esp32:
  board: esp32dev
  framework:
    type: esp-idf
external_components:
  - source:
      type: local
      path: SOURCE
    components: [znp_proxy]
wifi:
  ssid: compile-test
  password: compile-test-only
uart:
  id: radio
  tx_pin: GPIO17
  rx_pin:
    number: GPIO5
    ignore_strapping_warning: true
  baud_rate: 115200
  rx_buffer_size: 512B
  tx_buffer_size: 512B
znp_proxy:
  id: bridge
  uart_id: radio
  port: 6638
""".replace("SOURCE", str(SOURCE))


class ConfigurationTests(unittest.TestCase):
    def check_config(self, content, error=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.yaml"
            path.write_text(content)
            result = subprocess.run(
                [sys.executable, "-m", "esphome", "config", str(path)],
                capture_output=True,
                text=True,
                check=False,
            )
        output = result.stdout + result.stderr
        if error is None:
            self.assertEqual(result.returncode, 0, output)
        else:
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn(error, output)

    def test_valid(self):
        self.check_config(BASE)

    def test_unresponsive_automation(self):
        self.check_config(
            BASE + "  on_radio_unresponsive:\n    - delay: 15ms\n"
        )

    def test_wrong_baud(self):
        self.check_config(BASE.replace("115200", "460800"), "requires baud rate 115200")

    def test_small_buffers(self):
        self.check_config(BASE.replace("512B", "256B"), "at least 512B")

    def test_dummy_receiver(self):
        self.check_config(
            BASE.replace(
                "  baud_rate:", "  debug:\n    dummy_receiver: true\n  baud_rate:"
            ),
            "disable dummy_receiver",
        )

    def test_shared_uart(self):
        self.check_config(
            BASE + "\nuart_tcp:\n  uart_id: radio\n  role: server\n  port: 9999\n",
            "also used by 'uart_tcp'",
        )

    def test_shared_port(self):
        # Build the list explicitly instead of relying on YAML merge behavior.
        before, rest = BASE.split("uart:\n", 1)
        bus, after = rest.split("znp_proxy:\n", 1)
        config = before + "uart:\n  - " + bus[2:].replace("\n  ", "\n    ")
        config += "  - id: other\n    tx_pin: GPIO25\n    rx_pin: GPIO26\n    baud_rate: 115200\n"
        config += "znp_proxy:\n" + after
        config += "\nuart_tcp:\n  uart_id: other\n  role: server\n  port: 6638\n"
        self.check_config(config, "Port is already used")

    def test_timeout_range(self):
        self.check_config(BASE + "  response_timeout: 10ms\n", "at least")


if __name__ == "__main__":
    unittest.main()
