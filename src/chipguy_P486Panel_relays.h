/*
 * chipguy_P486Panel_relays - Relay driver for ESP32-P4-86-Panel board
 *
 * Simple relay control for the 2 relays on the Waveshare
 * ESP32-P4-86-Panel-ETH-2RO development board.
 *
 * Copyright (c) 2025 chipguyhere
 * MIT License
 */

#ifndef CHIPGUY_P486PANEL_RELAYS_H
#define CHIPGUY_P486PANEL_RELAYS_H

#include <Arduino.h>

class chipguy_P486Panel_relays {
public:
    chipguy_P486Panel_relays();

    // Initialize relay pins as outputs (all relays start OFF).
    //
    // Returns true. These relays are on plain GPIOs, so there is nothing to
    // probe and nothing that can fail -- but the signature matches every other
    // chipguy board/peripheral class, so a sketch can write
    // `if (!relays.begin()) ...` here exactly as it would on an I2C board.
    bool begin();

    // Set relay state: relayNumber 0-1, state 0=OFF, 1=ON (energized)
    void setRelay(uint8_t relayNumber, uint8_t state);

    // Energize relay (turn ON)
    void setRelayEnergized(uint8_t relayNumber);

    // Turn relay OFF
    void setRelayOff(uint8_t relayNumber);

private:
    static const uint8_t RELAY0_PIN = 32;
    static const uint8_t RELAY1_PIN = 46;

    uint8_t getPin(uint8_t relayNumber);
};

#endif // CHIPGUY_P486PANEL_RELAYS_H
