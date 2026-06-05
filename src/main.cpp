#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include "RS485Slave.h"
#include "FileSyncSlave.h"
#include "RelayView.h"
#include "json.h"
#include "SplashScreen.h"
#include "logo_gif.h"
#include "TopSequencer.h"

TFT_eSPI tft = TFT_eSPI();
JsonDocument doc;
RS485Slave slave;
FileSyncSlave fileSync(slave, "/plans");
RelayView     relayView(tft);
SplashScreen  splash(tft);
Top* tops = nullptr;
uint16_t      topCount = 0;
TopSequencer  sequencer;
State currentState = State::NotConnected;

//#define BUTTON_PIN 0  // physical PROG button
//#define MUX_PIN 25

//bool lastButtonState = HIGH;
//bool lastRelayState = LOW;

static constexpr uint8_t DETECT_PINS[RELAY_COUNT] = { 32, 33, 25, 23 };
static bool s_lastConnected[RELAY_COUNT] = {};

void checkIgniters()
{
    for (uint8_t i = 0; i < RELAY_COUNT; ++i)
    {
        // TODO: replace with digitalRead(DETECT_PINS[i]) == HIGH
        // when real hardware detection is ready
        const bool connected = (i == 0 || i == 1); // ← relay 0 always connected for testing

        if (connected != s_lastConnected[i])
        {
            s_lastConnected[i] = connected;
            relayView.setConnected(i, connected);
 
            Serial.printf("[App] Relay %u igniter %s\n",
                          i + 1, connected ? "connected" : "disconnected");
        }
    }
}

void drawState(State state) {
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String stateStr = StateToString(state);
  tft.drawString(stateStr, 320/2 - stateStr.length()/2, 20);
}

void loadTops(String fileName) {
  if(fileName != "" && LittleFS.exists("/plans/"+fileName)) {
    if(Json::loadFile(("/plans/"+fileName).c_str(), doc)) {
      JsonArray JsonTops = doc["tops"].as<JsonArray>();
      topCount = JsonTops.size();
      tops = new Top[topCount];
      int i = 0;
      for (JsonVariant value : JsonTops) {
        tops[i] = Top(
            value["id"]              | 0,
            value["timeInTenths"]    | 0
        );
        Serial.println(tops[i].id);
        i++;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  tft.init();
  tft.setRotation(1);

  if (!LittleFS.begin(false))
  {
      Serial.println("[FS] Mount failed, formatting...");
      LittleFS.format();
      if (!LittleFS.begin(true))
      {
          Serial.println("[FS] Mount failed after format — halting");
          while (true) delay(1000);
      }
  }
  Serial.println("[FS] LittleFS mounted");

  if (!LittleFS.exists("/plans"))
      LittleFS.mkdir("/plans");

  for (uint8_t i = 0; i < RELAY_COUNT; ++i)
    pinMode(DETECT_PINS[i], INPUT);

  splash.play(logo_gif, logo_gif_size);

  fileSync.onRegistered([](uint8_t assignedId)
  {
      Serial.printf("[App] Registered with master  id=0x%02X\n", assignedId);
  });

  slave.onCommand([](const char* cmd, JsonDocument& doc)
  {
    Serial.printf("[CMD] %s\n", cmd);

    if (strcmp(cmd, "set_led") == 0)
    {
        const char* color = doc["color"] | "none";
        const bool  blink = doc["blink"] | false;
        Serial.printf("  color=%s  blink=%d\n", color, blink);
        // TODO: drive your LED here
    }
  });

  slave.onFileReady([](const char* name, const uint8_t* data, size_t len)
  {
    Serial.printf("[FILE] '%s'  %zu bytes\n", name, len);
    // Preview first 64 bytes as text
    const size_t preview = min(len, static_cast<size_t>(64));
    Serial.write(data, preview);
    
    String path = "/";
    path += name;

    fs::File f = LittleFS.open(path, "w", true);
    if(!f) {
      Serial.printf("[FILE] Failed to open '%s' for writing\n", path.c_str());
      return;
    }

    const size_t written = f.write(data, len);
    f.close();

    if (written != len)
        Serial.printf("[FILE] Write incomplete: %zu/%zu bytes\n", written, len);
    else
        Serial.printf("[FILE] Saved '%s' OK (%zu bytes)\n", path.c_str(), written);

    // Verify it reads back correctly
    fs::File r = LittleFS.open(path, "r");
    Serial.printf("[FILE] Verified size on disk: %zu bytes\n", r.size());
    r.close();
  });

  // ── Sync callbacks ────────────────────────────────────────────────────────
  fileSync.onSyncComplete([]()
  {
      Serial.println("[App] Sync complete — all plans up to date");
      // TODO: signal readiness to the rest of the application
  });

  fileSync.begin();
  slave.begin();

  slave.onUnconnect([](){
    currentState = State::NotConnected;
    drawState(State::NotConnected);
  });

  slave.onPreparation([](){
    currentState = State::Preparation;
    relayView.begin();
  });

  slave.onArmement([](){
    currentState = State::Armement;
    drawState(State::Armement);
  });

  slave.onPlay([](String fileName) {
    currentState = State::Play;
    drawState(State::Play);
    
    if(tops == nullptr)
          loadTops(fileName);
    
    if(sequencer.isPaused()) {
      sequencer.resume();
    }
    else {
      sequencer.load(tops, topCount);
      sequencer.start();
    }
    
  });

  slave.onPause([](){
    currentState = State::Pause;
    drawState(State::Pause);

    if(sequencer.isRunning()) {
      sequencer.pause();
    }
  });

  slave.onStop([](){
    currentState = State::Stop;
    drawState(State::Stop);

    if(sequencer.isPaused() || sequencer.isRunning()) {
      sequencer.stop();
    }
  });

  slave.onFin([](){
    currentState = State::Fin;
    drawState(State::Fin);

    if(sequencer.isPaused() || sequencer.isRunning()) {
      sequencer.stop();
    }
  });

  slave.onJump([](int timeInTenth) {
    sequencer.jumpTo(timeInTenth);
  });

  sequencer.onFire([](int id)
  {
      Serial.printf("[App] Fire top id=%d\n", id);
      tft.drawString("fire " + String(id), 80, 120);
      // TODO: trigger relay for this id
  });
}

void loop() {

  slave.update();
  if(currentState == State::Preparation) {
    checkIgniters();
    relayView.update();
  }

  if(currentState == State::Play) {
    sequencer.update();
  }

  delay(1);
}