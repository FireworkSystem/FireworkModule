#include "FileSyncSlave.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / begin
// ─────────────────────────────────────────────────────────────────────────────

FileSyncSlave::FileSyncSlave(RS485Slave& slave, const char* planDir)
    : m_slave(slave)
    , m_planDir(planDir)
{}

void FileSyncSlave::begin()
{
    if (!LittleFS.exists(m_planDir))
        LittleFS.mkdir(m_planDir);

    Serial.printf("[SyncSlave] Ready  dir=%s\n", m_planDir);

    // ── Hook: handle application-level commands ───────────────────────────────
    m_slave.onCommand([this](const char* cmd, JsonDocument& doc)
    {
        if (strcmp(cmd, CMD_SYNC_LIST) == 0)
            handleSyncList(doc);
    });

    // ── Hook: save files received via the chunked file transfer ───────────────
    m_slave.onFileReady([this](const char* name, const uint8_t* data, size_t len)
    {
        saveFile(name, data, len);
    });

    // ── Hook: trigger sync automatically once registered ─────────────────────
    m_slave.onRegistered([this](uint8_t assignedId)
    {
        Serial.println("[SyncSlave] Registered — requesting sync");

        if (m_onRegistered)
            m_onRegistered(assignedId);

        requestSync();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Callback registration
// ─────────────────────────────────────────────────────────────────────────────

void FileSyncSlave::onSyncComplete(OnSyncComplete callback)
{
    m_onSyncComplete = callback;
}

void FileSyncSlave::onRegistered(OnRegistered callback)
{
    m_onRegistered = callback;
}


// ─────────────────────────────────────────────────────────────────────────────
//  requestSync
// ─────────────────────────────────────────────────────────────────────────────

void FileSyncSlave::requestSync()
{
    if (!m_slave.isRegistered())
    {
        Serial.println("[SyncSlave] requestSync before registration — ignored");
        return;
    }

    m_filesExpected = 0;
    m_filesReceived = 0;

    Serial.println("[SyncSlave] Sending sync_request");

    JsonDocument doc;
    doc["cmd"] = CMD_SYNC_REQUEST;
    m_slave.sendToMaster(doc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private — readLocalHash
// ─────────────────────────────────────────────────────────────────────────────

bool FileSyncSlave::readLocalHash(const char* path,
                                   char*       hashOut,
                                   size_t      hashOutSize) const
{
    hashOut[0] = '\0';

    File f = LittleFS.open(path, "r");
    if (!f)
        return false;

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err)
    {
        Serial.printf("[SyncSlave] JSON parse error in %s: %s\n",
                      path, err.c_str());
        return false;
    }

    const char* hash = doc["hash"] | "";
    if (strlen(hash) == 0)
        return false;

    strlcpy(hashOut, hash, hashOutSize);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private — parseSyncList
// ─────────────────────────────────────────────────────────────────────────────

uint8_t FileSyncSlave::parseSyncList(JsonArray& files,
                                      FileEntry* out,
                                      uint8_t    maxOut) const
{
    uint8_t count = 0;
    for (JsonObject entry : files)
    {
        if (count >= maxOut)
        {
            Serial.printf("[SyncSlave] Warning: more than %u files in sync_list"
                          " — extras ignored\n", maxOut);
            break;
        }

        const char* name = entry["name"] | "";
        const char* hash = entry["hash"] | "";

        if (strlen(name) == 0 || strlen(name) >= SYNC_MAX_FILENAME)
        {
            Serial.printf("[SyncSlave] Skipping entry with invalid name\n");
            continue;
        }
        //Serial.printf("%d, %d, %d", strlen(hash), SYNC_MAX_HASH, strlen(hash) >= SYNC_MAX_HASH);
        if (strlen(hash) == 0 || strlen(hash) > SYNC_MAX_HASH)
        {
            Serial.printf("[SyncSlave] Skipping '%s' — invalid hash\n", name);
            continue;
        }

        strlcpy(out[count].name, name, SYNC_MAX_FILENAME);
        strlcpy(out[count].hash, hash, SYNC_MAX_HASH);
        ++count;
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private — reconcile
// ─────────────────────────────────────────────────────────────────────────────

void FileSyncSlave::reconcile(const FileEntry* heltecFiles,
                               uint8_t          heltecCount,
                               NeededList&      needed) const
{
    needed.clear();

    // ── 1. Walk local LittleFS and delete files not in Heltec list ────────────
    File dir = LittleFS.open(m_planDir);
    if (dir && dir.isDirectory())
    {
        // Collect names to delete in a fixed array first — we must not delete
        // while iterating, as the directory cursor may become invalid.
        char toDelete[SYNC_MAX_FILES][SYNC_MAX_FILENAME] = {};
        uint8_t deleteCount = 0;

        File entry = dir.openNextFile();
        while (entry)
        {
            const char* name = entry.name();
            entry = dir.openNextFile();

            // Only consider JSON files
            const size_t nameLen = strlen(name);
            if (nameLen < 5 || strcmp(name + nameLen - 5, ".json") != 0)
                continue;

            // Check if this file exists on the Heltec
            bool foundOnHeltec = false;
            for (uint8_t i = 0; i < heltecCount; ++i)
            {
                if (strcmp(heltecFiles[i].name, name) == 0)
                {
                    foundOnHeltec = true;
                    break;
                }
            }

            if (!foundOnHeltec && deleteCount < SYNC_MAX_FILES)
                strlcpy(toDelete[deleteCount++], name, SYNC_MAX_FILENAME);
        }

        // Now delete the collected files
        for (uint8_t i = 0; i < deleteCount; ++i)
        {
            // Build full path: planDir + "/" + name
            char path[SYNC_MAX_FILENAME + 16] = {};
            snprintf(path, sizeof(path), "%s/%s", m_planDir, toDelete[i]);
            LittleFS.remove(path);
            Serial.printf("[SyncSlave] Deleted  %s  (not on Heltec)\n",
                          toDelete[i]);
        }
    }

    // ── 2. Check each Heltec file against local copies ────────────────────────
    for (uint8_t i = 0; i < heltecCount; ++i)
    {
        const char* name = heltecFiles[i].name;

        // Build full local path
        char localPath[SYNC_MAX_FILENAME + 16] = {};
        snprintf(localPath, sizeof(localPath), "%s/%s", m_planDir, name);

        char localHash[SYNC_MAX_HASH] = {};
        const bool exists = readLocalHash(localPath, localHash, sizeof(localHash));

        if (!exists)
        {
            Serial.printf("[SyncSlave] Missing    %s\n", name);
            needed.add(name);
        }
        else if (strcmp(localHash, heltecFiles[i].hash) != 0)
        {
            Serial.printf("[SyncSlave] Outdated   %s"
                          "  local=%s  heltec=%s\n",
                          name, localHash, heltecFiles[i].hash);
            needed.add(name);
        }
        else
        {
            Serial.printf("[SyncSlave] Up-to-date %s\n", name);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private — handleSyncList
// ─────────────────────────────────────────────────────────────────────────────

void FileSyncSlave::handleSyncList(JsonDocument& doc)
{
    Serial.println("[SyncSlave] Received sync_list");

    JsonArray heltecFilesJson = doc["files"].as<JsonArray>();
    if (heltecFilesJson.isNull())
    {
        Serial.println("[SyncSlave] sync_list has no files array — ignored");
        return;
    }

    // Parse into a fixed-size stack array — no heap involved
    FileEntry heltecFiles[SYNC_MAX_FILES];
    const uint8_t heltecCount = parseSyncList(heltecFilesJson,
                                               heltecFiles,
                                               SYNC_MAX_FILES);

    // Reconcile local state with Heltec list
    NeededList needed;
    reconcile(heltecFiles, heltecCount, needed);

    if (needed.empty())
    {
        Serial.println("[SyncSlave] Already in sync — nothing to download");
        if (m_onSyncComplete)
            m_onSyncComplete();
        return;
    }

    // Track progress so onSyncComplete fires after the last file
    m_filesExpected = needed.count;
    m_filesReceived = 0;

    // Build and send sync_need request
    Serial.printf("[SyncSlave] Requesting %u file(s) from master\n",
                  needed.count);

    JsonDocument request;
    request["cmd"] = CMD_SYNC_NEED;
    JsonArray filesArray = request["files"].to<JsonArray>();
    for (uint8_t i = 0; i < needed.count; ++i)
        filesArray.add(needed.names[i]);

    m_slave.sendToMaster(request);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private — saveFile
// ─────────────────────────────────────────────────────────────────────────────

void FileSyncSlave::saveFile(const char* name, const uint8_t* data, size_t len)
{
    // Ensure the plan directory exists — it may have been lost if the
    // filesystem was freshly formatted.
    if (!LittleFS.exists(m_planDir))
    {
        if (!LittleFS.mkdir(m_planDir))
        {
            Serial.printf("[SyncSlave] Failed to create dir '%s'\n", m_planDir);
            return;
        }
        Serial.printf("[SyncSlave] Created dir '%s'\n", m_planDir);
    }

    // Build full path on the stack
    char path[SYNC_MAX_FILENAME + 16] = {};
    snprintf(path, sizeof(path), "%s/%s", m_planDir, name);

    File f = LittleFS.open(path, "w", true);
    if (!f)
    {
        Serial.printf("[SyncSlave] Failed to open '%s' for writing\n", path);
        return;
    }

    const size_t written = f.write(data, len);
    f.close();

    if (written != len)
    {
        Serial.printf("[SyncSlave] Write incomplete: %zu/%zu B\n", written, len);
        return;
    }

    Serial.printf("[SyncSlave] Saved  %s  (%zu B)\n", path, len);

    // Fire sync complete once all expected files have arrived
    ++m_filesReceived;
    if (m_filesReceived >= m_filesExpected)
    {
        Serial.println("[SyncSlave] All files received — sync complete");
        if (m_onSyncComplete)
            m_onSyncComplete();
    }
}