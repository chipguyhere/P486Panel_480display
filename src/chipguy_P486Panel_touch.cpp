/*
 * GT911 capacitive touch driver for the Waveshare ESP32-P4-86-Panel, on the
 * board's shared I2C bus.  See chipguy_P486Panel_touch.h.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#ifdef ARDUINO_ESP32P4_DEV

#include "chipguy_P486Panel_touch.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "GT911";

#define GT911_READ_XY_REG       0x814E   // status: bit 7 = frame ready, low 4 bits = contacts
#define GT911_POINT1_REG        0x814F   // 8 bytes per contact
#define GT911_CONFIG_REG        0x8047
#define GT911_PRODUCT_ID_REG    0x8140

chipguy_P486Panel_touch::chipguy_P486Panel_touch(ThreadsafeWire &wire, int8_t rst_pin)
    : _wire(wire), _rst(rst_pin)
{
}

// The GT911's registers are 16-bit, big-endian on the wire, so the 8-bit
// readRegister() helper doesn't fit.  The Guard holds the bus across the
// register write, the repeated start and the drain of Wire's RX buffer.
bool chipguy_P486Panel_touch::_read(uint16_t reg, uint8_t *data, size_t len)
{
    ThreadsafeWire::Guard g(_wire);
    if (!g) return false;
    TwoWire &w = _wire.raw();
    w.beginTransmission(_addr);
    w.write((uint8_t)(reg >> 8));
    w.write((uint8_t)(reg & 0xFF));
    if (w.endTransmission(false) != 0) return false;
    if (w.requestFrom(_addr, len) != len) return false;
    for (size_t i = 0; i < len; i++) {
        int b = w.read();
        if (b < 0) return false;
        data[i] = (uint8_t)b;
    }
    return true;
}

bool chipguy_P486Panel_touch::_write(uint16_t reg, uint8_t value)
{
    ThreadsafeWire::Guard g(_wire);
    if (!g) return false;
    TwoWire &w = _wire.raw();
    w.beginTransmission(_addr);
    w.write((uint8_t)(reg >> 8));
    w.write((uint8_t)(reg & 0xFF));
    w.write(value);
    return w.endTransmission() == 0;
}

// A successful read of the product ID and config version is what counts as
// "the controller is up".
bool chipguy_P486Panel_touch::_identify(uint8_t addr)
{
    _addr = addr;
    uint8_t id[3], cfg;
    if (_read(GT911_PRODUCT_ID_REG, id, 3) && _read(GT911_CONFIG_REG, &cfg, 1)) {
        ESP_LOGI(TAG, "TouchPad_ID:0x%02x,0x%02x,0x%02x at 0x%02X, config version %d",
                 id[0], id[1], id[2], addr, cfg);
        return true;
    }
    _addr = 0;
    return false;
}

bool chipguy_P486Panel_touch::begin()
{
    _addr = 0;

    // Pulse reset (active low).  After a watchdog reset the GT911 has not been
    // power-cycled and may still be clamping SDA from a half-completed
    // transaction; the reset makes it let go of the bus.
    if (_rst >= 0) {
        gpio_config_t rst_conf = {
            .pin_bit_mask = 1ULL << _rst,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_conf);
        gpio_set_level((gpio_num_t)_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    delay(210);   // settle after reset before the first transaction

    // The GT911 may not respond after a soft reboot due to stale controller
    // state.  Keep retrying until it responds.
    for (int attempt = 1; attempt <= 20; attempt++) {
        if (_identify(P486Panel_Board::GT911_ADDR) ||
            _identify(P486Panel_Board::GT911_ALT_ADDR)) return true;
        ESP_LOGW(TAG, "GT911 init attempt %d failed, retrying...", attempt);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGE(TAG, "GT911 init failed after 20 attempts");
    return false;
}

uint8_t chipguy_P486Panel_touch::readPoints(uint16_t *x, uint16_t *y, uint8_t max)
{
    if (!_addr || max == 0) return 0;

    uint8_t status;
    if (!_read(GT911_READ_XY_REG, &status, 1)) return 0;

    // Bit 7: a new report is ready.  Low 4 bits: contacts in it.
    uint8_t n = status & 0x0F;
    if ((status & 0x80) == 0) {
        // No new report since the last read: the last one still stands,
        // unless the controller has gone quiet for too long.
        if (_lastN && millis() - _lastMs > HOLD_MS) _lastN = 0;
    } else if (n == 0 || n > MAX_POINTS) {
        _write(GT911_READ_XY_REG, 0);       // release the buffer for the next report
        _lastN = 0;                         // lifted (or garbage: treat as lifted)
    } else {
        // Per contact: track id, x lo/hi, y lo/hi, size lo/hi, reserved.
        uint8_t buf[8 * MAX_POINTS];
        bool ok = _read(GT911_POINT1_REG, buf, (size_t)n * 8);
        _write(GT911_READ_XY_REG, 0);       // release the buffer for the next report
        if (!ok) return 0;
        for (uint8_t i = 0; i < n; i++) {
            const uint8_t *p = &buf[i * 8];
            _lastX[i] = ((uint16_t)p[2] << 8) | p[1];
            _lastY[i] = ((uint16_t)p[4] << 8) | p[3];
        }
        _lastN = n;
        _lastMs = millis();
    }
    uint8_t k = _lastN < max ? _lastN : max;
    for (uint8_t i = 0; i < k; i++) { x[i] = _lastX[i]; y[i] = _lastY[i]; }
    return k;
}

bool chipguy_P486Panel_touch::read(uint16_t &x, uint16_t &y)
{
    return readPoints(&x, &y, 1) == 1;
}

#endif // ARDUINO_ESP32P4_DEV
