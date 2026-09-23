/*
 * chipguy_P486Panel_relays - Relay driver for ESP32-P4-86-Panel board
 *
 * Copyright (c) 2025 chipguyhere
 * MIT License
 */

#include "chipguy_P486Panel_relays.h"

chipguy_P486Panel_relays::chipguy_P486Panel_relays() {
}

bool chipguy_P486Panel_relays::begin() {
    pinMode(RELAY0_PIN, OUTPUT);
    pinMode(RELAY1_PIN, OUTPUT);

    digitalWrite(RELAY0_PIN, LOW);
    digitalWrite(RELAY1_PIN, LOW);

    return true;   // GPIO only: nothing to probe, nothing that can fail
}

uint8_t chipguy_P486Panel_relays::getPin(uint8_t relayNumber) {
    switch (relayNumber) {
        case 0: return RELAY0_PIN;
        case 1: return RELAY1_PIN;
        default: return 0;
    }
}

void chipguy_P486Panel_relays::setRelay(uint8_t relayNumber, uint8_t state) {
    uint8_t pin = getPin(relayNumber);
    if (pin) {
        digitalWrite(pin, state ? HIGH : LOW);
    }
}

void chipguy_P486Panel_relays::setRelayEnergized(uint8_t relayNumber) {
    setRelay(relayNumber, 1);
}

void chipguy_P486Panel_relays::setRelayOff(uint8_t relayNumber) {
    setRelay(relayNumber, 0);
}
