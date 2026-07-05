#pragma once

#include <FastLED.h>

// WS2811 LED on GPIO 1 showing the RS485 connection state to the master.
//   - Blinks red  → not connected / not registered
//   - Solid blue  → connected and registered
//
// NOTE: GPIO 1 is U0TXD on ESP32-WROOM-32 (shared with Serial).
// If serial output is garbled during LED updates, use a different GPIO.
class StatusLed
{
public:
    void begin();
    void setConnected(bool connected);
    void update();

private:
    static constexpr uint8_t  PIN        = 18;
    static constexpr uint8_t  NUM_LEDS   = 30;
    static constexpr uint32_t BLINK_MS   = 500;
    static constexpr uint8_t  BRIGHTNESS = 100;

    CRGB     m_leds[NUM_LEDS];
    bool     m_connected    = false;
    bool     m_blinkOn      = false;
    uint32_t m_lastToggleMs = 0;
};
