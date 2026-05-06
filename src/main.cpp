#include <Arduino.h>
#include <TFT_eSPI.h>
#include "Keypad.h"
#include "json.h"
#include "RS485Slave.h"
#include <FS.h>
#include <LittleFS.h>
#include "FileSyncSlave.h"

#define useKeypad   false

TFT_eSPI tft = TFT_eSPI();
Keypad   keypad(tft);
JsonDocument doc;
RS485Slave slave;
FileSyncSlave fileSync(slave, "/plans");

void setup() {
  Serial.begin(115200);
  tft.begin();
  
  delay(300);

  tft.setRotation(0);
  if(useKeypad) keypad.begin();

  tft.drawString("Hello", 0, 0);

  slave.onRegistered([](uint8_t assignedId)
  {
      Serial.printf("[App] Registered with master  id=0x%02X\n", assignedId);
      tft.drawString("registred", 0, 0);
      tft.drawString(String(assignedId), 0, 10);
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
    tft.drawString("file ready", 0, 0);
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
      Json::loadFile("/plans/Exemple TopTir.json", doc);
      tft.drawString(doc["hash"] | "", 0, 20);
      // TODO: signal readiness to the rest of the application
  });

  // sync.begin() registers all internal hooks including onRegistered,
  // onCommand, and onFileReady — call it before slave.begin() so the
  // hooks are in place before the first frame can arrive.
  fileSync.begin();


  slave.begin();
} 

void loop() {

  if(useKeypad) {
    keypad.update();

    int value;
    if (keypad.getConfirmedValue(value)) {
      Serial.printf("Confirmed: %d\n", value);
    }
  }

  slave.update(); // must be called every iteration
  delay(1);
}