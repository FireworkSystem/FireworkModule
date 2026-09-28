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
#include "StatusLed.h"

TFT_eSPI tft = TFT_eSPI();
JsonDocument doc;
RS485Slave slave(Serial2, /*TX*/17, /*RX*/18, /*DE*/RS485_NO_DE_PIN, /*baud*/9600);
StatusLed  statusLed;
FileSyncSlave fileSync(slave, "/plans");
RelayView     relayView(tft);
SplashScreen  splash(tft);
Top* tops = nullptr;
uint16_t      topCount = 0;
TopSequencer  sequencer;
State currentState = State::NotConnected; // to reduce call to getCurrentState() in update()

static constexpr uint8_t DETECT_PINS[RELAY_COUNT]  = {  };
static bool s_lastConnected[RELAY_COUNT]           = {};
static bool s_pendingConfigLoad                    = false;

static constexpr uint8_t UPSTREAM_SIGNAL_PIN       = 16; // driven HIGH to signal presence to upstream
static constexpr uint8_t DOWNSTREAM_PRESENCE_PIN  = 21; // read HIGH when a downstream module is present
static bool s_lastDownstream                       = false;

// Shows a full-screen popup in portrait mode asking whether to restore the saved
// relay config. Returns true if the user taps YES or the 5-second countdown
// expires (auto-load); false if the user taps NO.
// Must be called after relayView.calibrateTouch() so touch coords are valid.
static bool showConfigPopup()
{
    // landscape 320×240 — same rotation as the splash screen
    tft.fillScreen(TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&FreeSansBold18pt7b);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("Config found!", 160, 35);
    tft.drawString("Reload relay", 160, 78);
    tft.drawString("assignments?", 160, 108);

    tft.drawFastHLine(0, 160, 320, TFT_WHITE);

    // YES button — left half
    tft.fillRect(0, 161, 160, 79, TFT_DARKGREEN);
    tft.drawFastVLine(159, 161, 79, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString("YES", 80, 200);

    // NO button — right half
    tft.fillRect(160, 161, 160, 79, 0xC000); // dark red
    tft.setTextColor(TFT_WHITE, 0xC000);
    tft.drawString("NO", 240, 200);

    const uint32_t start   = millis();
    uint32_t       lastSec = 99;

    while (true)
    {
        const uint32_t elapsed = millis() - start;
        if (elapsed >= 5000) return true; // auto-load

        const uint32_t remaining = (5000 - elapsed + 999) / 1000;
        if (remaining != lastSec)
        {
            lastSec = remaining;
            tft.fillRect(0, 130, 320, 26, TFT_NAVY);
            tft.setTextColor(TFT_YELLOW, TFT_NAVY);
            tft.drawString("Auto-load in " + String(remaining) + "s", 160, 143);
        }

        uint16_t tx = 0, ty = 0;
        if (tft.getTouch(&tx, &ty))
        {
            // Calibration was stored at rotation 0 (portrait).
            // Convert portrait coords to landscape: lx = ty, ly = 239 - tx.
            uint16_t lx = ty;
            uint16_t ly = 239 - tx;
            if (ly >= 160)
            {
                delay(50); // debounce
                return lx < 160; // YES = left, NO = right
            }
        }
        delay(20);
    }
}

void checkDownstream()
{
    const bool downstream = digitalRead(DOWNSTREAM_PRESENCE_PIN) == HIGH;
    if (downstream != s_lastDownstream)
    {
        s_lastDownstream = downstream;
        statusLed.setConnected(downstream);
        Serial.printf("[App] Downstream module %s\n", downstream ? "connected" : "disconnected");
    }
}

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
  delay(2000);
  
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

  relayView.calibrateTouch();
  if (LittleFS.exists(RELAY_CONFIG_PATH))
      s_pendingConfigLoad = showConfigPopup();

  //for (uint8_t i = 0; i < RELAY_COUNT; ++i)
  //  pinMode(DETECT_PINS[i], INPUT);  // GPIO32/33 reserved for flash on ESP32-S3

  pinMode(UPSTREAM_SIGNAL_PIN, OUTPUT);
  digitalWrite(UPSTREAM_SIGNAL_PIN, HIGH);

  pinMode(DOWNSTREAM_PRESENCE_PIN, INPUT_PULLDOWN);

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
  fileSync.onTransferStart([](uint8_t total)
  {
      Serial.printf("[App] Downloading %u file(s) from master\n", total);
      statusLed.startTransfer();
  });

  fileSync.onSyncComplete([]()
  {
      Serial.println("[App] Sync complete — all plans up to date");
      statusLed.endTransfer();
  });

  statusLed.begin();

  fileSync.begin();
  slave.begin();

  slave.onUnconnect([](){
    // if we are currently playing don't stop and continue without master
    if(currentState != State::Play) {
      currentState = State::NotConnected;
      drawState(State::NotConnected);
    }
  });

  slave.onPreparation([](){
    currentState = State::Preparation;
    relayView.begin();
    if (s_pendingConfigLoad)
    {
        relayView.loadConfig(RELAY_CONFIG_PATH);
        s_pendingConfigLoad = false;
    }
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

  slave.onJump([](int timeInTenths) {
    sequencer.jumpTo(timeInTenths);
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
  checkDownstream();
  statusLed.update();
  if(currentState == State::Preparation) {
    checkIgniters();
    relayView.update();
  }

  if(currentState == State::Play) {
    sequencer.update();
  }

  delay(1);
}