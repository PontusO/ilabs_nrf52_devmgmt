// iLabs LTE FOTA -- QSPI storage layer.
//
// Partition-aware wrapper around Adafruit_SPIFlash. All public methods
// take SLOT-RELATIVE offsets (0..slot_size-1), never raw QSPI byte
// addresses. The class internally adds the slot base. This makes it
// impossible for caller code to accidentally write outside its
// intended partition.
//
// Single source of truth for the QSPI memory map: ilabs_fota_settings.h
// (a verbatim mirror of the bootloader contract header). The kSlot* /
// kSettings* constants here are aliases into those #defines so the
// wrapper stays one cp away from the bootloader.
//
// When this code is extracted to the iLabs_LteFota Arduino library,
// this header lives at `src/ilabs_fota_qspi.h`. Constants stay
// `constexpr`-in-class so callers don't depend on a global namespace.
//
// Dependency: Adafruit_SPIFlash + Adafruit_FlashTransport_QSPI (both
// ship with the iLabs nRF52 Arduino BSP).

#ifndef ILABS_FOTA_QSPI_H
#define ILABS_FOTA_QSPI_H

#include <stdint.h>
#include <stddef.h>

// Use the *Base* class (raw flash chip API only). Adafruit_SPIFlash.h
// pulls in SdFat headers that require ENABLE_EXTENDED_TRANSFER_CLASS
// and FAT12_SUPPORT to be set in SdFatConfig.h. We don't want any FAT
// filesystem -- just raw sector erase / page write / read -- and the
// base header gives us exactly that with no extra dependencies.
#include <Adafruit_SPIFlashBase.h>

#include "ilabs_fota_settings.h"
#include "ilabs_fota_slot.h"

class iLabsFotaQspi {
public:
    // ---- Memory-map constants ----
    // All sizes/offsets are 4 KB-sector aligned. Public so test code
    // can assert against them. Values come from the bootloader-side
    // contract header (ilabs_fota_settings.h) -- single source of truth.
    static constexpr uint32_t kSectorSize         = ILABS_FOTA_SETTINGS_SECTOR_SIZE;
    static constexpr uint32_t kPageSize           = 0x0100;     // 256 B

    static constexpr uint32_t kSettingsAAddr      = ILABS_FOTA_SETTINGS_A_ADDR;
    static constexpr uint32_t kSettingsBAddr      = ILABS_FOTA_SETTINGS_B_ADDR;
    static constexpr uint32_t kSettingsSize       = kSectorSize;

    static constexpr uint32_t kDownloadSlotAddr   = ILABS_FOTA_DOWNLOAD_SLOT_ADDR;
    // Usable size; the nominal 1 MB slot loses 2 sectors to the
    // settings double-buffer at the top of the partition.
    static constexpr uint32_t kDownloadSlotSize   = ILABS_FOTA_DOWNLOAD_SLOT_USABLE;

    static constexpr uint32_t kBackupSlotAddr     = ILABS_FOTA_BACKUP_SLOT_ADDR;
    static constexpr uint32_t kBackupSlotSize     = ILABS_FOTA_BACKUP_SLOT_USABLE;

    // Upper bound for the raw user-region API (rawErase/rawWrite/rawRead):
    // the start of the FOTA reservation. Anything at or above this is
    // backup/download/settings and must never be touched by raw writes.
    // Derived from the bootloader partition map, so the library owns this
    // bound with no dependency on any log/user-partition header.
    static constexpr uint32_t kUserRegionBytes    = kBackupSlotAddr;

    // ---- Lifecycle ----

    iLabsFotaQspi();
    ~iLabsFotaQspi() = default;

    // Initialise the underlying flash interface. Returns true if the
    // chip is detected (JEDEC ID matches a known SPI flash). Safe to
    // call more than once; later calls are no-ops.
    bool begin();

    // True once begin() has succeeded and the chip responded.
    bool ready() const { return _ready; }

    // ---- Low-power suspend / resume ----
    //
    // Put the W25Q64 into chip-level deep-power-down (~1 µA) AND
    // disable the nRF52840 QSPI peripheral (drops NRF_QSPI->ENABLE to
    // 0, saves ~150-300 µA of peripheral baseline). Call this when no
    // FOTA staging is in progress -- which is the steady state, since
    // FOTAs are infrequent. ready() returns false after suspend().
    //
    // Re-call resume() before any read / write / erase operation. If
    // the orchestrator forgets, every other public method returns
    // false because `_ready` is false. Calling suspend() when already
    // suspended is a no-op; same for resume() when already live.
    void suspend();
    void resume();

    // Reported chip capacity in bytes (queried from JEDEC ID).
    // Useful for boot-time sanity check that we got the expected chip.
    uint32_t chipSize() const;

    // ---- Download slot (application writes during OTA staging) ----

    // Erase the entire download slot (~1 MB, ~40 s wall-clock). Most
    // call sites should NOT use this -- the FOTA orchestrator erases
    // sectors just-in-time as the download progresses. Useful for
    // bench scrubbing.
    bool eraseDownloadSlot();

    // Erase one 4 KB sector. slot_offset must be sector-aligned and
    // strictly less than kDownloadSlotSize.
    bool eraseDownloadSector(uint32_t slot_offset);

    // Write `len` bytes to the download slot at slot_offset. Caller is
    // responsible for ensuring the relevant sector(s) have been erased
    // beforehand. `slot_offset + len` must be <= kDownloadSlotSize.
    // Returns true on success.
    bool writeDownload(uint32_t slot_offset, const uint8_t* buf, size_t len);

    // Read `len` bytes from the download slot. Bounds-checked.
    bool readDownload(uint32_t slot_offset, uint8_t* buf, size_t len) const;

    // ---- Backup slot (read only from application) ----

    // Backup operations are intentionally limited to read on the
    // application side. The bootloader has its own write path. The
    // application can use this for diagnostics ("does the backup look
    // intact?").
    bool readBackup(uint32_t slot_offset, uint8_t* buf, size_t len) const;

    // ---- Settings block (transactional, double-buffered) ----

    // Read the active settings block (= the one with higher sequence
    // number AND a valid CRC). On success, fills *out and returns true.
    // If neither block has a valid CRC, returns false -- caller should
    // treat as "uninitialised" and either fall back to defaults or
    // start a fresh block at sequence=1 via writeSettings().
    bool readSettings(ilabs_fota_settings_t* out) const;

    // Write `*in` to the inactive block (the one with the LOWER
    // sequence number or whichever is invalid). Internally bumps
    // `sequence` to (active.sequence + 1) and computes the CRC32, plus
    // overwrites magic/struct_size/struct_version with the canonical
    // values so callers don't have to remember to set them. Other
    // fields in `*in` are programmed verbatim. Returns true on success.
    //
    // Atomicity guarantee: if power is lost mid-write, the previously
    // active block is left intact; the next boot's readSettings()
    // returns the older-but-valid state.
    bool writeSettings(const ilabs_fota_settings_t* in);

    // ---- High-level FOTA control APIs ----
    //
    // Thin convenience wrappers over readSettings()/writeSettings() that
    // encode the contract from ilabs_fota_settings.h. They're the
    // intended entry points for orchestrator + sketch code; the raw
    // read/write methods are there for diagnostics and edge cases.

    // Set ILABS_FOTA_FLAG_UPDATE_PENDING and stage the version field
    // that announces what's waiting in the download slot. Clears
    // swap_retry_count and last_status_code so the bootloader sees a
    // fresh attempt. Returns true on success. If the prior settings
    // block is corrupt, a fresh one is written at sequence=1 with the
    // pending bit set.
    //
    // The caller is responsible for having already written the slot
    // header + payload into the download slot before invoking this. The
    // bootloader treats UPDATE_PENDING as a commit point.
    bool triggerUpdate(uint32_t download_fw_version);

    // ---- Raw user-region access (for in-house partitions outside the
    //      FOTA reservation, e.g. the log_manager partition) ----
    //
    // Three thin pass-throughs over Adafruit_SPIFlashBase, bounds-
    // checked against kUserRegionBytes so a misbehaving client
    // can't stomp on the FOTA region. The caller owns its own QSPI
    // resume/suspend lifecycle -- these methods assume QSPI is
    // already active (caller has called resume() recently).
    //
    // Address is absolute (0..kUserRegionBytes-1). Erase
    // requires sector alignment. Write does NOT erase first; the
    // caller is responsible for sector erasure before writing.

    bool rawErase(uint32_t abs_addr);
    bool rawWrite(uint32_t abs_addr, const uint8_t* buf, size_t len);
    bool rawRead (uint32_t abs_addr, uint8_t* buf, size_t len) const;

    // ---- Hash utilities (exposed; also used internally) ----

    // CRC-32/IEEE (zlib's crc32, same polynomial Linux uses) -- the
    // settings-block hash. Also handy for any caller that wants to
    // verify settings/slot bytes it just read.
    static uint32_t crc32_ieee(const uint8_t* data, size_t len);

    // CRC-16-CCITT (poly 0x1021, init 0xFFFF, no final XOR) -- the
    // slot-header hash, per ilabs_fota_slot.h. The orchestrator uses
    // this to validate downloaded slot headers; the host-side signing
    // tool must match exactly (Python: crcmod.predefined "xmodem", or
    // bit-banged with the same parameters).
    static uint16_t crc16_ccitt(const uint8_t* data, size_t len);

private:
    Adafruit_FlashTransport_QSPI _transport;
    Adafruit_SPIFlashBase        _flash;
    bool                         _ready;

    // Internal helpers (private; never expose raw addresses upward).
    bool erase_one_sector_abs(uint32_t abs_addr);
    bool write_abs(uint32_t abs_addr, const uint8_t* buf, size_t len);
    bool read_abs(uint32_t abs_addr, uint8_t* buf, size_t len) const;
};

// Pre-instantiated global. Same pattern as Adafruit's `flash` /
// Arduino's `Serial` / `Wire`. Application code uses this directly.
extern iLabsFotaQspi FotaQspi;

#endif // ILABS_FOTA_QSPI_H
