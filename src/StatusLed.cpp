#include "StatusLed.h"

void StatusLed::begin()
{
    FastLED.addLeds<WS2811, PIN, RGB>(m_leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
    fill_solid(m_leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

void StatusLed::setConnected(bool connected)
{
    m_connected = connected;
    if (connected)
    {
        fill_solid(m_leds, NUM_LEDS, CRGB::Green);
        FastLED.show();
    }
}

void StatusLed::update()
{
    if (m_connected)
        return;

    const uint32_t now = millis();
    if (now - m_lastToggleMs >= BLINK_MS)
    {
        m_lastToggleMs = now;
        m_blinkOn      = !m_blinkOn;
        fill_solid(m_leds, NUM_LEDS, m_blinkOn ? CRGB::Red : CRGB::Black);
        FastLED.show();
    }
}
