import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_ZNP_PROXY_ID, ZnpProxy

DEPENDENCIES = ["znp_proxy"]

CONFIG_SCHEMA = {
    cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
    cv.GenerateID(CONF_ZNP_PROXY_ID): cv.use_id(ZnpProxy),
    cv.Optional("connected"): binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_CONNECTIVITY,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
}


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ZNP_PROXY_ID])
    sensors = binary_sensor.sub_binary_sensors(config)
    await sensors("connected", hub.set_connected_sensor)
