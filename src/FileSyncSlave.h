#pragma once

#include "RS485Slave.h"

#include <FS.h>
#include <LittleFS.h>

// ─────────────────────────────────────────────────────────────────────────────
//  FileSyncSlave
//
//  Runs on the ESP32 WROOM.
//
//  Responsibilities:
//    - After registration, send a sync_request to the Heltec
//    - Receive the sync_list and compare it against local LittleFS files
//    - Delete local files whose hash is not in the Heltec list
//    - Request missing or outdated files via sync_need
//    - Receive files via the existing RS485Slave file transfer and save them
//
//  Memory model
//    No heap allocation — all intermediate state uses fixed-size stack arrays.
//    SYNC_MAX_FILES controls how many plan files are supported simultaneously.
//    SYNC_MAX_FILENAME and SYNC_MAX_HASH bound name and hash string lengths.
//
//  Usage
//    FileSyncSlave sync(slave);
//    sync.begin();              // call once in setup() before slave.begin()
//
//    // In loop() — nothing needed, hooks into slave callbacks
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t SYNC_MAX_FILES    = 16;  ///< Max simultaneous plan files
static constexpr uint8_t SYNC_MAX_FILENAME = 48;  ///< Max filename length (with null)
static constexpr uint8_t SYNC_MAX_HASH     = 64;  ///< Max hash string length (with null)

class FileSyncSlave
{
public:
    using OnSyncComplete = std::function<void()>;

    /// @param slave    The RS485Slave instance.
    /// @param planDir  LittleFS directory that mirrors the Heltec plan files.
    explicit FileSyncSlave(RS485Slave& slave,
                           const char* planDir = "/plans");

    /// Register hooks on the slave and create the plan directory if needed.
    /// Call once in setup(), BEFORE slave.begin().
    void begin();

    /// Called when sync completes — either nothing to download, or the last
    /// file has been saved successfully.
    void onSyncComplete(OnSyncComplete callback);

    /// Manually trigger a sync (sends sync_request to master).
    /// Called automatically after registration; call again if needed.
    void requestSync();

private:
    // ── Fixed-size file descriptor ────────────────────────────────────────────

    struct FileEntry
    {
        char name[SYNC_MAX_FILENAME] = {};
        char hash[SYNC_MAX_HASH]     = {};
    };

    // ── Fixed-size list of files to request ──────────────────────────────────

    struct NeededList
    {
        char     names[SYNC_MAX_FILES][SYNC_MAX_FILENAME] = {};
        uint8_t  count                                    = 0;

        void clear()                      { count = 0; }
        bool full()  const                { return count >= SYNC_MAX_FILES; }
        bool empty() const                { return count == 0; }

        /// Add a filename — silently ignored if the list is full or name is too long.
        void add(const char* name)
        {
            if (full() || strlen(name) >= SYNC_MAX_FILENAME)
                return;
            strlcpy(names[count++], name, SYNC_MAX_FILENAME);
        }
    };

    // ── Helpers ───────────────────────────────────────────────────────────────

    /// Read the "hash" field from a local JSON file into @p hashOut.
    /// @return true on success.
    bool readLocalHash(const char* path, char* hashOut, size_t hashOutSize) const;

    /// Compare the received file list against LittleFS:
    ///   - Deletes local files not present on the Heltec
    ///   - Fills @p needed with filenames that are missing or outdated
    void reconcile(const FileEntry* heltecFiles,
                   uint8_t          heltecCount,
                   NeededList&      needed) const;

    /// Parse the sync_list JSON into a fixed FileEntry array.
    /// @return Number of entries parsed (capped at SYNC_MAX_FILES).
    uint8_t parseSyncList(JsonArray& files,
                          FileEntry* out,
                          uint8_t    maxOut) const;

    /// Handle an incoming sync_list command from the master.
    void handleSyncList(JsonDocument& doc);

    /// Save a received file to LittleFS (hooked into slave.onFileReady).
    void saveFile(const char* name, const uint8_t* data, size_t len);

    // ── Members ───────────────────────────────────────────────────────────────
    RS485Slave&    m_slave;
    const char*    m_planDir;          // points to a string literal — no heap
    OnSyncComplete m_onSyncComplete;

    // Track how many files are still outstanding so we know when sync is done
    uint8_t        m_filesExpected = 0;
    uint8_t        m_filesReceived = 0;
};