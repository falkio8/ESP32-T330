"""ESPHome External Component - Landis+Gyr T330 Heat Meter."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_WATER,
    DEVICE_CLASS_DURATION,
    STATE_CLASS_TOTAL_INCREASING,
    STATE_CLASS_MEASUREMENT,
    UNIT_KILOWATT_HOURS,
    UNIT_CELSIUS,
    UNIT_HOUR,
    UNIT_SECOND,
)

AUTO_LOAD = ["sensor", "text_sensor", "network"]
CODEOWNERS = ["@local"]

t330_ns = cg.esphome_ns.namespace("t330_meter")
T330Component = t330_ns.class_("T330Component", cg.PollingComponent)
RealTimeClock = cg.esphome_ns.namespace("time").class_("RealTimeClock", cg.Component)

CONF_TX_PIN            = "tx_pin"
CONF_RX_PIN            = "rx_pin"
CONF_ENERGY_KWH        = "energy_kwh"
CONF_VOLUME_QM         = "volume_qm"
CONF_POWER_W           = "power_w"
CONF_VOLUME_FLOW       = "volume_flow"
CONF_FLOW_TEMP         = "flow_temp"
CONF_RETURN_TEMP       = "return_temp"
CONF_TEMP_DIFF         = "temp_diff"
CONF_OPERATING_TIME    = "operating_time"
CONF_ACTIVITY_DURATION = "activity_duration"
CONF_FABRICATION_NO    = "fabrication_number"
CONF_LAST_READ         = "last_read"
CONF_READ_STATUS       = "read_status"
CONF_TIME_ID           = "time_id"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(T330Component),
            cv.Optional(CONF_TX_PIN, default=4):  cv.positive_int,
            cv.Optional(CONF_RX_PIN, default=36): cv.positive_int,
            cv.Optional(CONF_ENERGY_KWH): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOWATT_HOURS,
                accuracy_decimals=3,
                device_class=DEVICE_CLASS_ENERGY,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:radiator",
            ),
            cv.Optional(CONF_VOLUME_QM): sensor.sensor_schema(
                unit_of_measurement="m\u00b3",
                accuracy_decimals=4,
                device_class=DEVICE_CLASS_WATER,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:water",
            ),
            cv.Optional(CONF_POWER_W): sensor.sensor_schema(
                unit_of_measurement="W",
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_POWER,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:fire",
            ),
            cv.Optional(CONF_VOLUME_FLOW): sensor.sensor_schema(
                unit_of_measurement="m\u00b3/h",
                accuracy_decimals=4,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:water-pump",
            ),
            cv.Optional(CONF_FLOW_TEMP): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:thermometer-chevron-up",
            ),
            cv.Optional(CONF_RETURN_TEMP): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:thermometer-chevron-down",
            ),
            cv.Optional(CONF_TEMP_DIFF): sensor.sensor_schema(
                unit_of_measurement="K",
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:thermometer-lines",
            ),
            cv.Optional(CONF_OPERATING_TIME): sensor.sensor_schema(
                unit_of_measurement=UNIT_HOUR,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:clock-outline",
            ),
            cv.Optional(CONF_ACTIVITY_DURATION): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:timer",
            ),
            cv.Optional(CONF_FABRICATION_NO): text_sensor.text_sensor_schema(
                icon="mdi:barcode",
                entity_category="diagnostic",
            ),
            cv.Optional(CONF_LAST_READ): text_sensor.text_sensor_schema(
                icon="mdi:clock-check-outline",
                entity_category="diagnostic",
            ),
            cv.Optional(CONF_READ_STATUS): text_sensor.text_sensor_schema(
                icon="mdi:check-network-outline",
                entity_category="diagnostic",
            ),
            cv.Optional(CONF_TIME_ID): cv.use_id(RealTimeClock),
        }
    )
    .extend(cv.polling_component_schema("30min"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_tx_pin(config[CONF_TX_PIN]))
    cg.add(var.set_rx_pin(config[CONF_RX_PIN]))

    if CONF_ENERGY_KWH in config:
        s = await sensor.new_sensor(config[CONF_ENERGY_KWH])
        cg.add(var.set_energy_kwh_sensor(s))
    if CONF_VOLUME_QM in config:
        s = await sensor.new_sensor(config[CONF_VOLUME_QM])
        cg.add(var.set_volume_qm_sensor(s))
    if CONF_POWER_W in config:
        s = await sensor.new_sensor(config[CONF_POWER_W])
        cg.add(var.set_power_w_sensor(s))
    if CONF_VOLUME_FLOW in config:
        s = await sensor.new_sensor(config[CONF_VOLUME_FLOW])
        cg.add(var.set_volume_flow_sensor(s))
    if CONF_FLOW_TEMP in config:
        s = await sensor.new_sensor(config[CONF_FLOW_TEMP])
        cg.add(var.set_flow_temp_sensor(s))
    if CONF_RETURN_TEMP in config:
        s = await sensor.new_sensor(config[CONF_RETURN_TEMP])
        cg.add(var.set_return_temp_sensor(s))
    if CONF_TEMP_DIFF in config:
        s = await sensor.new_sensor(config[CONF_TEMP_DIFF])
        cg.add(var.set_temp_diff_sensor(s))
    if CONF_OPERATING_TIME in config:
        s = await sensor.new_sensor(config[CONF_OPERATING_TIME])
        cg.add(var.set_operating_time_sensor(s))
    if CONF_ACTIVITY_DURATION in config:
        s = await sensor.new_sensor(config[CONF_ACTIVITY_DURATION])
        cg.add(var.set_activity_duration_sensor(s))
    if CONF_FABRICATION_NO in config:
        s = await text_sensor.new_text_sensor(config[CONF_FABRICATION_NO])
        cg.add(var.set_fabrication_sensor(s))
    if CONF_LAST_READ in config:
        s = await text_sensor.new_text_sensor(config[CONF_LAST_READ])
        cg.add(var.set_last_read_sensor(s))
        # Verknüpfe die time-Komponente (homeassistant) mit der C++-Klasse
        if CONF_TIME_ID in config:
            time_ = await cg.get_variable(config[CONF_TIME_ID])
            cg.add(var.set_time(time_))
    if CONF_READ_STATUS in config:
        s = await text_sensor.new_text_sensor(config[CONF_READ_STATUS])
        cg.add(var.set_read_status_sensor(s))
