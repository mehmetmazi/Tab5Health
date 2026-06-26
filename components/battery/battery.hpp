#pragma once
#include "esp_err.h"

// Initialise the INA226 power monitor on the Tab5 internal I2C bus
// (SCL = GPIO32, SDA = GPIO31, addr 0x41). Call once at startup.
esp_err_t battery_init();

// Battery / system bus voltage in volts (from the INA226), or < 0 on error.
float battery_voltage();
