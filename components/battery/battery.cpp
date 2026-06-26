/*
 * Battery monitor — reads the Tab5's INA226 over the internal I2C bus.
 * SCL=GPIO32, SDA=GPIO31, INA226 @0x41 (per M5Stack HAL; 0x40 is a different chip).
 * Bus-voltage register 0x02 (1.25 mV/LSB) — no calibration needed.
 *
 * That internal bus is shared with ~10 chips and gets wedged under heavy network
 * load (clear-bus/reset can't recover it), so on a failed read we tear the bus
 * down and rebuild it before retrying.
 */
#include "battery.hpp"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "battery";
static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_ina = nullptr;

static bool create_bus()
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;                 // M5GFX uses I2C_NUM_1 (touch) — 0 is free
    bus_cfg.sda_io_num = GPIO_NUM_31;
    bus_cfg.scl_io_num = GPIO_NUM_32;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) { s_bus = nullptr; return false; }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = 0x41;               // INA226 on the Tab5
    dev_cfg.scl_speed_hz    = 400000;
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_ina) != ESP_OK) { s_ina = nullptr; return false; }

    uint8_t cfg[3] = { 0x00, 0x45, 0x27 };        // avg 16, 1.1 ms conv, continuous shunt+bus
    i2c_master_transmit(s_ina, cfg, sizeof cfg, 100);
    return true;
}

static void destroy_bus()
{
    if (s_ina) { i2c_master_bus_rm_device(s_ina); s_ina = nullptr; }
    if (s_bus) { i2c_del_master_bus(s_bus);       s_bus = nullptr; }
}

esp_err_t battery_init()
{
    // The shared bus throws expected "clear bus failed" errors we recover from —
    // quiet the driver so the logs stay readable.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    if (!create_bus()) { ESP_LOGE(TAG, "INA226 init failed"); return ESP_FAIL; }
    ESP_LOGI(TAG, "INA226 ready (SCL32/SDA31 @0x41)");
    return ESP_OK;
}

float battery_voltage()
{
    if (!s_ina && !create_bus()) return -1.0f;

    uint8_t reg = 0x02, data[2] = { 0, 0 };
    esp_err_t r = i2c_master_transmit_receive(s_ina, &reg, 1, data, 2, 100);
    if (r != ESP_OK) {                            // bus wedged — rebuild it, retry once
        destroy_bus();
        if (create_bus())
            r = i2c_master_transmit_receive(s_ina, &reg, 1, data, 2, 100);
    }
    if (r != ESP_OK) { ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(r)); return -1.0f; }

    uint16_t raw = (uint16_t(data[0]) << 8) | data[1];
    return raw * 0.00125f;                         // bus voltage: 1.25 mV/LSB
}
