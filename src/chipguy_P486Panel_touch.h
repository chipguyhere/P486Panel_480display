/*
 * GT911 capacitive touch driver for the Waveshare ESP32-P4-86-Panel, on the
 * board's shared I2C bus.
 *
 * The GT911 sits on GPIO7/8 with the audio codec, the microphone ADC and any
 * camera.  That bus belongs to P486Panel_Board (chipguy_P486Panel_board), as a
 * mutex-protected ThreadsafeWire, so this driver takes the bus rather than
 * opening its own.  lv_setup does this for you:
 *
 *     P486Panel_Board board;
 *     chipguy_P486Panel_touch touch(board.Wire);
 *     board.begin();
 *     touch.begin();
 *
 * Reports contacts in NATIVE panel coordinates (720x720, no mirroring).
 * There is no interrupt line (the GT911's INT goes to a test point), so
 * touch is polled.
 *
 * Register map and read sequence follow Goodix's GT911 programming guide.  An
 * earlier version of this driver was derived from Espressif's esp_lcd_touch /
 * esp_lcd_touch_gt911 components (Apache-2.0); this is a rewrite on the shared
 * bus and contains none of that code.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#ifdef ARDUINO_ESP32P4_DEV

#include <Arduino.h>
#include <P486Panel_Board.h>   // ThreadsafeWire, pin and address map

// Kept for sketches that size arrays with it.
#define TOUCH_MAX_POINTS 5

class chipguy_P486Panel_touch {
public:
    static constexpr uint8_t MAX_POINTS = TOUCH_MAX_POINTS;

    chipguy_P486Panel_touch(ThreadsafeWire &wire,
                            int8_t rst_pin = P486Panel_Board::TOUCH_RST_PIN);

    // Pulses reset, then retries until the controller answers (20 x 250 ms):
    // after a soft reboot the GT911 can take a while to respond.  Tries both
    // GT911 addresses.  Returns false if it never answers; read() then reports
    // no touches.  The bus must already be started (board.begin()).
    bool begin();

    // First contact, in native 720x720 coordinates.  False when not touched.
    bool read(uint16_t &x, uint16_t &y);

    // Up to `max` contacts (at most MAX_POINTS), in native coordinates.
    // Returns how many were filled in; 0 when not touched.  Safe to call at
    // any rate: between the controller's reports (about every 10 ms) it
    // returns the last report again, and a touch counts as lifted when the
    // controller says so, or after HOLD_MS with no report at all.
    uint8_t readPoints(uint16_t *x, uint16_t *y, uint8_t max);

    bool ok() const { return _addr != 0; }
    uint8_t address() const { return _addr; }

private:
    ThreadsafeWire &_wire;
    int8_t  _rst;
    uint8_t _addr = 0;
    static constexpr uint32_t HOLD_MS = 100;
    uint8_t  _lastN = 0;
    uint16_t _lastX[MAX_POINTS], _lastY[MAX_POINTS];
    uint32_t _lastMs = 0;

    bool _read(uint16_t reg, uint8_t *data, size_t len);
    bool _write(uint16_t reg, uint8_t value);
    bool _identify(uint8_t addr);
};

#endif // ARDUINO_ESP32P4_DEV
