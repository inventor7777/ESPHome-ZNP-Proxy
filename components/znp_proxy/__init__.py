import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import socket, uart
from esphome.const import CONF_ID, CONF_PORT, CONF_UART_ID

DEPENDENCIES = ["esp32", "network", "uart"]
AUTO_LOAD = ["socket"]
CODEOWNERS = []

CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_ZNP_PROXY_ID = "znp_proxy_id"
znp_proxy_ns = cg.esphome_ns.namespace("znp_proxy")
ZnpProxy = znp_proxy_ns.class_("ZnpProxy", cg.Component, uart.UARTDevice)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ZnpProxy),
            cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_PORT, default=6638): cv.port,
            cv.Optional(CONF_RESPONSE_TIMEOUT, default="500ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=250),
                    max=cv.TimePeriod(milliseconds=1000),
                ),
            ),
            cv.Optional("allowed_ips"): socket.IPV4_ALLOW_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    socket.consume_sockets(1, "znp_proxy", socket.SocketType.TCP),
    socket.consume_sockets(1, "znp_proxy", socket.SocketType.TCP_LISTEN),
)


def _validate_uart(config):
    if config.get("debug", {}).get("dummy_receiver", False):
        raise cv.Invalid(
            "znp_proxy requires exclusive UART input; disable dummy_receiver"
        )
    if config.get("tx_buffer_size", 0) < 512 or config.get("rx_buffer_size", 0) < 512:
        raise cv.Invalid(
            "znp_proxy requires UART tx_buffer_size and rx_buffer_size of at least 512B"
        )
    return config


def _final_validate(config):
    uart.final_validate_device_schema(
        "znp_proxy",
        baud_rate=115200,
        data_bits=8,
        parity="NONE",
        stop_bits=1,
        require_tx=True,
        require_rx=True,
    )(config)
    fv.id_declaration_match_schema(_validate_uart)(config[CONF_UART_ID])
    full = fv.full_config.get()

    # Match every declared uart_id, including inside nested platform configs.
    def references(value):
        if isinstance(value, dict):
            return value.get(CONF_UART_ID) == config[CONF_UART_ID] or any(
                references(v) for v in value.values()
            )
        return isinstance(value, list) and any(references(v) for v in value)

    for domain, value in full.items():
        if domain != "znp_proxy" and references(value):
            raise cv.Invalid(
                f"UART is also used by '{domain}'; remove its reader/writer before using znp_proxy"
            )
    for domain in ("uart_tcp", "tcp_uart"):
        for other in full.get(domain, []):
            if other.get("role") == "server" and other[CONF_PORT] == config[CONF_PORT]:
                raise cv.Invalid(f"Port is already used by {domain}")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    socket.require_tcp_listener()
    socket.add_ipv4_allow(var.set_allow, config.get("allowed_ips"), config[CONF_ID])
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
