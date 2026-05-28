#include "RS485Slave.h"
#include <esp_mac.h>   // esp_read_mac()

// ─────────────────────────────────────────────────────────────────────────────
//  FileState::reset
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::FileState::reset()
{
    active      = false;
    name[0]     = '\0';
    totalSize   = 0;
    totalChunks = 0;
    nextChunk   = 0;
    received    = 0;

    if (buf)
    {
        free(buf);
        buf = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────

RS485Slave::RS485Slave(HardwareSerial& serial,
                       uint8_t         pinTx,
                       uint8_t         pinRx,
                       uint8_t         pinDE,
                       uint32_t        baud,
                       uint32_t        helloRetryMs,
                       uint32_t        connectionTimeoutMs)
    : RS485Node(serial, pinTx, pinRx, pinDE, baud)
    , m_helloRetryMs(helloRetryMs)
    , m_connectionTimeoutMs(connectionTimeoutMs)
{   
    // Read MAC once at construction time
    uint8_t rawMac[6];
    esp_read_mac(rawMac, ESP_MAC_WIFI_STA);
    snprintf(m_mac, sizeof(m_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             rawMac[0], rawMac[1], rawMac[2],
             rawMac[3], rawMac[4], rawMac[5]);
}

RS485Slave::~RS485Slave()
{
    m_file.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::begin()
{
    RS485Node::begin();
    m_lastContactMs = millis(); // start timeout clock from boot
    Serial.printf("[Slave] RS485 ready  TX=%d  RX=%d  DE=%d  @%u baud  MAC=%s\n",
                  m_pinTx, m_pinRx, m_pinDE, m_baud, m_mac);
    sendHello();
}

void RS485Slave::update()
{
    // ── 1. Process any waiting incoming frame ─────────────────────────────────
    RS485Frame frame;
    if (readFrame(frame, 20))
    {
        // Accept frames addressed to our assigned ID, unregistered address, or broadcast
        const bool forUs = (frame.dst == m_addr)
                        || (frame.dst == NODE_UNREGISTERED_ADDR && !m_registered)
                        || (frame.dst == RS485_BROADCAST);

        if (forUs)
        {
            // Any valid frame from master resets the connection timeout
            m_lastContactMs = millis();

            JsonDocument doc;
            const DeserializationError err = deserializeJson(
                doc,
                reinterpret_cast<const char*>(frame.payload),
                frame.payloadLen);

            if (err)
            {
                Serial.printf("[Slave] JSON error: %s\n", err.c_str());
                sendNack("json_error");
            }
            else
            {
                const char* cmd = doc["cmd"] | "";
                handleCommand(cmd, doc);
            }
        }
    }

    // ── 2. Connection timeout — master went away ───────────────────────────────
    // If registered but no frame received within the timeout window,
    // assume the master has restarted and re-enter discovery.
    if (m_registered &&
        millis() - m_lastContactMs >= m_connectionTimeoutMs)
    {
        Serial.printf("[Slave] No contact from master for %ums — re-registering\n",
                      m_connectionTimeoutMs);

        // Reset registration state
        m_registered = false;
        m_addr       = 0;
        m_file.reset();

        // Send hello immediately — don't wait for the retry timer
        sendHello();
    }

    // ── 3. Retry hello if still unregistered ──────────────────────────────────
    if (!m_registered &&
        millis() - m_lastHelloMs >= m_helloRetryMs)
    {
        Serial.println("[Slave] No welcome received — retrying hello");

        if(m_onUnconnect)
            m_onUnconnect();

        sendHello();
    }
}

void RS485Slave::goodbye()
{
    if (!m_registered)
        return;

    JsonDocument doc;
    doc["cmd"] = CMD_GOODBYE;
    doc["mac"] = m_mac;
    sendToMaster(doc);

    Serial.printf("[Slave] Sent goodbye  id=0x%02X  MAC=%s\n", m_addr, m_mac);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Callback registration
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::onCommand   (OnCommand    cb) { m_onCommand    = cb; }
void RS485Slave::onFileReady (OnFileReady  cb) { m_onFileReady  = cb; }
void RS485Slave::onRegistered(OnRegistered cb) { m_onRegistered = cb; }
void RS485Slave::onUnconnect(OnUnconnect cb) { m_onUnconnect = cb; }
void RS485Slave::onPreparation(OnPreparation cb) { m_onPreparation = cb; }
void RS485Slave::onArmement(OnArmement cb) { m_onArmement = cb; }
void RS485Slave::onPlay(OnPlay cb) { m_onPlay = cb; }
void RS485Slave::onPause(OnPause cb) { m_onPause = cb; }
void RS485Slave::onStop(OnStop cb) { m_onStop = cb; }
void RS485Slave::onFin(OnFin cb) { m_onFin = cb; }

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

bool RS485Slave::sendToMaster(JsonDocument& doc)
{
    if (!m_registered)
    {
        Serial.println("[Slave] sendToMaster called before registration — ignored");
        return false;
    }

    String json;
    serializeJson(doc, json);
    return sendFrame(RS485_MASTER_ADDR,
                     m_addr,
                     reinterpret_cast<const uint8_t*>(json.c_str()),
                     static_cast<uint16_t>(json.length()));
}

bool    RS485Slave::isRegistered() const { return m_registered; }
uint8_t RS485Slave::address()      const { return m_addr; }
const char* RS485Slave::mac()      const { return m_mac; }

// ─────────────────────────────────────────────────────────────────────────────
//  Discovery
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::sendHello()
{
    Serial.printf("[Slave] Sending hello frame, DE=%d\n",
                  digitalRead(m_pinDE)); // should be HIGH during TX, LOW after
    setTxMode();
    Serial.printf("[Slave] DE after setTxMode: %d\n", digitalRead(m_pinDE));
    
    JsonDocument doc;
    doc["cmd"] = CMD_HELLO;
    doc["mac"] = m_mac;

    String json;
    serializeJson(doc, json);

    // Send from NODE_UNREGISTERED_ADDR until we have a real ID
    sendFrame(RS485_MASTER_ADDR,
              NODE_UNREGISTERED_ADDR,
              reinterpret_cast<const uint8_t*>(json.c_str()),
              static_cast<uint16_t>(json.length()));

    m_lastHelloMs = millis();
    Serial.printf("[Slave] hello sent  MAC=%s\n", m_mac);
}

void RS485Slave::handleWelcome(JsonDocument& doc)
{
    // The welcome is broadcast to all unregistered slaves simultaneously.
    // Each slave must verify the MAC field matches its own before accepting —
    // otherwise two simultaneous hellos would cause both slaves to adopt the
    // same assigned ID.
    const char* welcomeMac = doc["mac"] | "";
    if (strncmp(welcomeMac, m_mac, sizeof(m_mac)) != 0)
    {
        Serial.printf("[Slave] welcome is for MAC=%s — not mine (%s), ignored\n",
                      welcomeMac, m_mac);
        return;
    }

    const uint8_t assignedId = doc["id"] | static_cast<uint8_t>(0);
    if (assignedId == 0)
    {
        Serial.println("[Slave] welcome received but id=0 — ignored");
        return;
    }

    m_addr       = assignedId;
    m_registered = true;

    Serial.printf("[Slave] Registered!  MAC=%s  assigned id=0x%02X\n",
                  m_mac, m_addr);

    if (m_onRegistered)
        m_onRegistered(m_addr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ACK / NACK
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::sendAck(const char* info)
{
    JsonDocument doc;
    doc["cmd"]  = CMD_ACK;
    doc["addr"] = m_addr;
    if (info && strlen(info) > 0)
        doc["info"] = info;
    sendToMaster(doc);
}

void RS485Slave::sendNack(const char* reason)
{
    JsonDocument doc;
    doc["cmd"]    = CMD_NACK;
    doc["addr"]   = m_addr;
    doc["reason"] = reason;
    sendToMaster(doc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Command dispatcher
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::handleCommand(const char* cmd, JsonDocument& doc)
{
    Serial.printf("[Slave 0x%02X] cmd=%s\n", m_addr, cmd);

    if (strcmp(cmd, CMD_WELCOME) == 0)
    {
        handleWelcome(doc);
    }
    else if (strcmp(cmd, CMD_PING) == 0)
    {
        sendAck(CMD_PONG);
    }
    else if (strcmp(cmd, CMD_FILE_START) == 0)
    {
        handleFileStart(doc);
    }
    else if (strcmp(cmd, CMD_FILE_CHUNK) == 0)
    {
        handleFileChunk(doc);
    }
    else if (strcmp(cmd, CMD_FILE_END) == 0)
    {
        handleFileEnd(doc);
    }
    else if (strcmp(cmd, CMD_SEND_STATE) == 0) {
        handleStateChange(doc);
    }
    else
    {
        // Forward to the user callback for application commands
        if (m_onCommand)
            m_onCommand(cmd, doc);
        sendAck();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  File transfer — start
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::handleFileStart(JsonDocument& doc)
{
    m_file.reset();

    strlcpy(m_file.name, doc["name"] | "unknown", sizeof(m_file.name));
    m_file.totalSize   = doc["size"]   | 0;
    m_file.totalChunks = doc["chunks"] | 0;

    if (m_file.totalSize == 0)
    {
        sendNack("bad_size");
        return;
    }

    m_file.buf = static_cast<uint8_t*>(malloc(m_file.totalSize));
    if (!m_file.buf)
    {
        sendNack("no_mem");
        return;
    }

    m_file.active    = true;
    m_file.nextChunk = 0;
    m_file.received  = 0;

    Serial.printf("[Slave 0x%02X] File start  name='%s'  size=%zu  chunks=%u\n",
                  m_addr, m_file.name, m_file.totalSize, m_file.totalChunks);

    sendAck("ready");
}

// ─────────────────────────────────────────────────────────────────────────────
//  File transfer — chunk
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::handleFileChunk(JsonDocument& doc)
{
    if (!m_file.active)
    {
        sendNack("no_transfer");
        return;
    }

    const uint16_t idx   = doc["idx"] | static_cast<uint16_t>(0xFFFF);
    const uint16_t rxCRC = doc["crc"] | static_cast<uint16_t>(0);
    const char*    b64   = doc["data"] | "";

    if (idx != m_file.nextChunk)
    {
        Serial.printf("[Slave 0x%02X] Out-of-order chunk %u (expected %u)\n",
                      m_addr, idx, m_file.nextChunk);
        sendNack("out_of_order");
        return;
    }

    uint8_t    tmp[RS485_MAX_PAYLOAD];
    const size_t chunkLen = base64Decode(b64, strlen(b64), tmp, sizeof(tmp));

    if (chunkLen == 0)
    {
        sendNack("b64_error");
        return;
    }

    if (rs485ComputeCRC(tmp, chunkLen) != rxCRC)
    {
        sendNack("crc_mismatch");
        return;
    }

    if (m_file.received + chunkLen > m_file.totalSize)
    {
        sendNack("overflow");
        return;
    }

    memcpy(m_file.buf + m_file.received, tmp, chunkLen);
    m_file.received  += chunkLen;
    m_file.nextChunk++;

    Serial.printf("[Slave 0x%02X] Chunk %u/%u OK\n",
                  m_addr, m_file.nextChunk, m_file.totalChunks);
    sendAck();
}

// ─────────────────────────────────────────────────────────────────────────────
//  File transfer — end
// ─────────────────────────────────────────────────────────────────────────────

void RS485Slave::handleFileEnd(JsonDocument& doc)
{
    if (!m_file.active)
    {
        sendNack("no_transfer");
        return;
    }

    const uint16_t rxTotalCRC   = doc["total_crc"] | static_cast<uint16_t>(0);
    const uint16_t calcTotalCRC = rs485ComputeCRC(m_file.buf, m_file.received);

    if (calcTotalCRC != rxTotalCRC)
    {
        Serial.printf("[Slave 0x%02X] File CRC mismatch  calc=0x%04X  rx=0x%04X\n",
                      m_addr, calcTotalCRC, rxTotalCRC);
        m_file.reset();
        sendNack("file_crc_fail");
        return;
    }

    Serial.printf("[Slave 0x%02X] File '%s' complete  %zu B  CRC OK\n",
                  m_addr, m_file.name, m_file.received);

    if (m_onFileReady)
        m_onFileReady(m_file.name, m_file.buf, m_file.received);

    sendAck("file_ok");
    m_file.reset();
}

void RS485Slave::handleStateChange(JsonDocument& doc) {
    int intState = doc["state"];
    State state = static_cast<State>(intState);
    switch(state) {
        case State::NotConnected: {
            if(m_onUnconnect)
                m_onUnconnect();
            break;
        }
        case State::Preparation: {
            if(m_onPreparation)
                m_onPreparation();
            break;
        }
        case State::Armement: {
            if(m_onArmement)
                m_onArmement();
            break;
        }
        case State::Play: {
            String fileName = doc["filename"];
            if(m_onPlay)
                m_onPlay(fileName);
            break;
        }
        case State::Pause: {
            if(m_onPause)
                m_onPause();
            break;
        }
        case State::Stop: {
            if(m_onStop)
                m_onStop();
            break;
        }
        case State::Fin: {
            if(m_onFin)
                m_onFin();
            break;
        }
    }
}