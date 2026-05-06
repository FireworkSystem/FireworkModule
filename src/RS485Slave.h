#pragma once

#include "RS485Node.h"
#include "NodeRegistry.h"

#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  RS485Slave  –  ESP32 WROOM
//
//  Wiring
//    MAX485 DI   ← GPIO 17  (TX / Serial2)
//    MAX485 RO   → GPIO 16  (RX / Serial2)
//    MAX485 DE+RE← GPIO 26  (direction)
//    VCC 3.3 V / GND
//
//  Discovery lifecycle
//    1. On begin(), the slave reads its own MAC and sends a "hello" to master.
//    2. Master replies with a "welcome" containing the assigned bus ID.
//    3. The slave adopts that ID and begins normal operation.
//    4. On clean shutdown, call goodbye() to notify the master immediately.
//
//  Usage
//    RS485Slave slave;           // no address needed — assigned by master
//    slave.onCommand(…);
//    slave.onFileReady(…);
//    slave.onRegistered(…);      // fired when master assigns an ID
//    slave.begin();
//
//    // In loop():
//    slave.update();
//
//    // On clean shutdown:
//    slave.goodbye();
// ─────────────────────────────────────────────────────────────────────────────

class RS485Slave : public RS485Node
{
public:
    // ── Callback signatures ───────────────────────────────────────────────────

    /// Invoked for every application-level command not handled internally.
    using OnCommand = std::function<void(const char* cmd, JsonDocument& doc)>;

    /// Invoked once a complete file has been received and its CRC verified.
    using OnFileReady = std::function<void(const char*    name,
                                           const uint8_t* data,
                                           size_t         len)>;

    /// Invoked when the master has assigned a bus ID to this slave.
    using OnRegistered = std::function<void(uint8_t assignedId)>;

    // ── Constructor ───────────────────────────────────────────────────────────

    /// @param helloRetryMs   How long to wait before retrying the hello if
    ///                       no welcome is received (ms).
    explicit RS485Slave(HardwareSerial& serial        = Serial2,
                        uint8_t         pinTx         = 17,
                        uint8_t         pinRx         = 16,
                        uint8_t         pinDE         = 26,
                        uint32_t        baud          = 115200,
                        uint32_t        helloRetryMs  = 3000);

    /// Destructor — releases any pending file buffer.
    ~RS485Slave() override;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Initialise GPIO, UART, and send the first "hello".  Call once in setup().
    void begin() override;

    /// Process incoming frames and retry hello if not yet registered.
    /// Must be called every loop() iteration.
    void update();

    /// Send a "goodbye" to the master (call before powering down).
    void goodbye();

    // ── Callback registration ─────────────────────────────────────────────────
    void onCommand   (OnCommand    callback);
    void onFileReady (OnFileReady  callback);
    void onRegistered(OnRegistered callback);

    // ── Public API ────────────────────────────────────────────────────────────

    /// Send an unsolicited JSON message to the master.
    /// Only valid after the slave has been registered (isRegistered() == true).
    bool sendToMaster(JsonDocument& doc);

    /// @return true once the master has assigned a bus ID.
    bool    isRegistered() const;

    /// @return The assigned bus address (0 if not yet registered).
    uint8_t address() const;

    /// @return This board's WiFi MAC as "AA:BB:CC:DD:EE:FF".
    const char* mac() const;

private:
    // ── Discovery ─────────────────────────────────────────────────────────────
    void sendHello();
    void handleWelcome(JsonDocument& doc);

    // ── Internal file-transfer state ─────────────────────────────────────────
    struct FileState
    {
        bool     active      = false;
        char     name[64]    = {};
        size_t   totalSize   = 0;
        uint16_t totalChunks = 0;
        uint16_t nextChunk   = 0;
        uint8_t* buf         = nullptr;
        size_t   received    = 0;

        void reset();
    };

    // ── Command routing ───────────────────────────────────────────────────────
    void sendAck (const char* info = "");
    void sendNack(const char* reason);

    void handleCommand  (const char* cmd, JsonDocument& doc);
    void handleFileStart(JsonDocument& doc);
    void handleFileChunk(JsonDocument& doc);
    void handleFileEnd  (JsonDocument& doc);

    // ── Members ───────────────────────────────────────────────────────────────
    uint8_t  m_addr         = 0;              ///< 0 until master assigns an ID
    char     m_mac[18]      = {};             ///< "AA:BB:CC:DD:EE:FF"
    bool     m_registered   = false;

    uint32_t m_helloRetryMs;
    uint32_t m_lastHelloMs  = 0;

    OnCommand    m_onCommand;
    OnFileReady  m_onFileReady;
    OnRegistered m_onRegistered;
    FileState    m_file;
};