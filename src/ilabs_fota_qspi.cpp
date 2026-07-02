// iLabs LTE FOTA -- QSPI storage layer (implementation).
//
// See ilabs_fota_qspi.h for the public API contract.
//
// Library extraction note: this file moves verbatim to
// `src/ilabs_fota_qspi.cpp` when extracted. The only sketch-level
// dependency is `Serial` for log output; replace with a logging hook
// at extraction time if the library shouldn't directly own a print
// stream.

#include "ilabs_fota_qspi.h"
#include "ilabs_fota_log.h"
#include <nrfx.h>
#include <hal/nrf_qspi.h>

#include <Arduino.h>
#include <string.h>

#include <SPI.h>
// Adafruit_FlashTransport_QSPI is pulled in transitively through
// Adafruit_SPIFlash.h (in our header). Don't include it directly --
// the public path through the library is the umbrella header only.

// CRC32 covers the struct minus its trailing crc32 field, per the
// bootloader contract (ilabs_fota_settings.h).
static constexpr uint32_t kSettingsCrcRange =
    ILABS_FOTA_SETTINGS_SIZE - sizeof(uint32_t);

// -------- global instance --------

iLabsFotaQspi FotaQspi;

// -------- ctor --------

iLabsFotaQspi::iLabsFotaQspi()
    : _transport(),
      _flash(&_transport),
      _ready(false) {}

// -------- begin / chip query --------

bool iLabsFotaQspi::begin() {
    if (_ready) return true;

    // Wake the W25Q64 from any prior Deep Power-Down before the JEDEC
    // read. The chip is on a separate power rail and is NOT affected
    // by an MCU reset, so a soft reset (NVIC_SystemReset, watchdog,
    // hang) leaves the chip in DPD from the previous session's
    // FotaQspi.suspend(). In DPD the chip ignores every opcode except
    // 0xAB (release), which means a 0x9F JEDEC read returns 0xFF and
    // flash.begin() silently fails. Pre-wake removes the hazard with
    // a ~5 µs cost on every boot (harmless when the chip is already
    // awake).
    _transport.begin();                 // bring QSPI peripheral up
    _transport.runCommand(0xAB);        // Release from DPD
    delayMicroseconds(5);               // W25Q64JV tRES1 (datasheet)

    if (!_flash.begin()) {
        LOG_ERROR("[fota-qspi] flash.begin() failed");
        return false;
    }
    _ready = true;

    LOG_INFO("[fota-qspi] JEDEC chip detected, %lu B (%lu MB)",
             (unsigned long)_flash.size(),
             (unsigned long)(_flash.size() / (1024UL * 1024UL)));

    // Sanity: the bootloader contract assumes an 8 MB part because the
    // settings sectors live at the very top. Anything smaller silently
    // truncates writes -- refuse to operate.
    if (_flash.size() < ILABS_FOTA_QSPI_SIZE) {
        LOG_ERROR("[fota-qspi] FATAL: flash too small for partition map");
        _ready = false;
        return false;
    }
    return true;
}

// Reference count for nested resume()/suspend() pairs. Hardware is
// only acted on when the count crosses zero. This lets a "batch"
// caller (log_manager's flush task) wrap many internal QSPI ops --
// including those that go through Adafruit_LittleFS bd callbacks
// which themselves resume/suspend per op -- without paying the
// chip wake/sleep latency on every nested call.
static int s_resume_depth = 0;

// Power model: full peripheral teardown on suspend, full re-init on
// resume. Restores the ~150-300 µA peripheral-baseline savings.
//
// The underlying bug: stock nrfx_qspi_uninit() triggers
// TASKS_DEACTIVATE then IMMEDIATELY clears ENABLE without waiting
// for EVENTS_READY. The peripheral ends up half-deactivated; the
// next nrfx_qspi_init() returns success and qspi_ready_wait() after
// ACTIVATE returns OK, but JEDEC reads then return 0xFF and
// flash.begin() silently fails ("not detected").
//
// The iLabs BSP patches this in nrfx_qspi.c (2026-06-18). Once a
// build is using the patched BSP the issue is gone at the source.
//
// The pre-DEACTIVATE+wait we still do in suspend() below is now
// belt-and-suspenders: it makes this code keep working when built
// against an UNPATCHED upstream nrfx (e.g. before the BSP package
// is re-released and installed via the Boards Manager). The two
// fixes coexist safely -- double-DEACTIVATE is harmless because the
// second one runs on an already-deactivated peripheral and is a no-op.
// Future cleanup: once every deployed unit is on a BSP version that
// includes the patch, the app-side workaround here can be removed.

void iLabsFotaQspi::suspend() {
    if (s_resume_depth == 0) return;        // unbalanced; no-op
    s_resume_depth--;
    if (s_resume_depth > 0) return;         // still nested; keep hw alive
    if (!_ready) return;

    // 1. Put the chip into Deep Power-Down (~1 µA).
    _transport.runCommand(0xB9);
    delayMicroseconds(5);                   // W25Q64JV tDP

    // 2. Manually deactivate the QSPI peripheral with proper wait.
    //    Without this, nrfx_qspi_uninit's bug strands the peripheral
    //    in a state from which the next init cannot recover.
    nrf_qspi_event_clear(NRF_QSPI, NRF_QSPI_EVENT_READY);
    nrf_qspi_task_trigger(NRF_QSPI, NRF_QSPI_TASK_DEACTIVATE);
    {
        // ~10 ms cap. The peripheral normally completes deactivation
        // in microseconds; the timeout is just a safety net so we
        // don't spin forever if the hardware is wedged.
        uint32_t spin = 100000;
        while (!nrf_qspi_event_check(NRF_QSPI, NRF_QSPI_EVENT_READY)
               && spin-- > 0) {
            // tight poll
        }
    }

    // 3. Now safe to tear down via Adafruit_FlashTransport_QSPI::end()
    //    -> nrfx_qspi_uninit(). Drops NRF_QSPI->ENABLE to 0 and
    //    applies Anomaly 122 ("current consumption too high") fix.
    _flash.end();

    _ready = false;
}

void iLabsFotaQspi::resume() {
    s_resume_depth++;
    if (s_resume_depth > 1) return;         // already brought up by outer caller
    if (_ready) return;

    // Wake-from-DPD sequence. Peripheral comes up first via
    // _transport.begin() (nrfx_qspi_init), then 0xAB tells the chip
    // to leave DPD, then _flash.begin() does JEDEC re-detect.
    //
    // The post-0xAB delay was 5 µs (W25Q64JV tRES1, DPD->standby). That
    // proved marginal: over thousands of suspend/resume cycles the
    // JEDEC read inside _flash.begin() occasionally landed before the
    // chip was fully awake and flash.begin() failed. The datasheet's
    // tRES1 is 3 µs but release-to-first-valid-access has more margin in
    // practice, so use 50 µs -- still negligible, well clear of any
    // realistic wake time. This stops the intermittent resume failures
    // that (before the LOG_* suspend guard) could hang the flush task.
    _transport.begin();

    // Retry the DPD-release + JEDEC re-detect. A single 0xAB + 50 us settle is
    // usually enough, but it still occasionally fails -- the JEDEC read inside
    // flash.begin() lands before the chip is fully awake. Previously a one-shot
    // failure left the WHOLE QSPI log store dead until the next resume: writes
    // silently dropped, the upload high-water couldn't be persisted, and a log
    // slice got re-uploaded (duplicate members server-side). Re-issue 0xAB with
    // a growing settle and retry a few times before giving up -- 0xAB and
    // flash.begin() are both idempotent, so retrying is safe.
    for (int attempt = 1; attempt <= 5; attempt++) {
        _transport.runCommand(0xAB);          // release Deep Power-Down
        delayMicroseconds(50 * attempt);      // 50,100,150,200,250 us growing margin
        if (_flash.begin()) {
            _ready = true;
            return;
        }
    }

    LOG_ERROR("[fota-qspi] resume: flash.begin() failed after 5 attempts");
    s_resume_depth--;                         // failed -- back out (suspend() no-ops at 0)
}

uint32_t iLabsFotaQspi::chipSize() const {
    return const_cast<Adafruit_SPIFlashBase&>(_flash).size();
}

// -------- low-level helpers (private) --------

bool iLabsFotaQspi::erase_one_sector_abs(uint32_t abs_addr) {
    if (!_ready) return false;
    if (abs_addr & (kSectorSize - 1)) return false;     // alignment check
    uint32_t sector = abs_addr / kSectorSize;
    return _flash.eraseSector(sector);
}

bool iLabsFotaQspi::write_abs(uint32_t abs_addr,
                                const uint8_t* buf, size_t len) {
    if (!_ready || buf == nullptr || len == 0) return false;
    size_t written = _flash.writeBuffer(abs_addr, buf, len);
    return written == len;
}

bool iLabsFotaQspi::read_abs(uint32_t abs_addr,
                               uint8_t* buf, size_t len) const {
    if (!_ready || buf == nullptr || len == 0) return false;
    size_t got = const_cast<Adafruit_SPIFlashBase&>(_flash)
                     .readBuffer(abs_addr, buf, len);
    return got == len;
}

// -------- raw user-region access (bounds-checked) --------


bool iLabsFotaQspi::rawErase(uint32_t abs_addr) {
    if (abs_addr + kSectorSize > kUserRegionBytes) return false;
    return erase_one_sector_abs(abs_addr);
}

bool iLabsFotaQspi::rawWrite(uint32_t abs_addr,
                              const uint8_t* buf, size_t len) {
    if (abs_addr + len > kUserRegionBytes) return false;
    return write_abs(abs_addr, buf, len);
}

bool iLabsFotaQspi::rawRead(uint32_t abs_addr,
                             uint8_t* buf, size_t len) const {
    if (abs_addr + len > kUserRegionBytes) return false;
    return read_abs(abs_addr, buf, len);
}

// -------- download slot --------

bool iLabsFotaQspi::eraseDownloadSlot() {
    if (!_ready) return false;
    for (uint32_t o = 0; o < kDownloadSlotSize; o += kSectorSize) {
        if (!erase_one_sector_abs(kDownloadSlotAddr + o)) {
            LOG_ERROR("[fota-qspi] erase failed at slot offset 0x%lX",
                      (unsigned long)o);
            return false;
        }
    }
    return true;
}

bool iLabsFotaQspi::eraseDownloadSector(uint32_t slot_offset) {
    if (slot_offset >= kDownloadSlotSize) return false;
    return erase_one_sector_abs(kDownloadSlotAddr + slot_offset);
}

bool iLabsFotaQspi::writeDownload(uint32_t slot_offset,
                                    const uint8_t* buf, size_t len) {
    if ((uint64_t)slot_offset + len > kDownloadSlotSize) return false;
    return write_abs(kDownloadSlotAddr + slot_offset, buf, len);
}

bool iLabsFotaQspi::readDownload(uint32_t slot_offset,
                                   uint8_t* buf, size_t len) const {
    if ((uint64_t)slot_offset + len > kDownloadSlotSize) return false;
    return read_abs(kDownloadSlotAddr + slot_offset, buf, len);
}

// -------- backup slot (read only from application) --------

bool iLabsFotaQspi::readBackup(uint32_t slot_offset,
                                 uint8_t* buf, size_t len) const {
    if ((uint64_t)slot_offset + len > kBackupSlotSize) return false;
    return read_abs(kBackupSlotAddr + slot_offset, buf, len);
}

// -------- settings block --------

// Compute IEEE 802.3 CRC32 (zlib's crc32, same polynomial Linux uses).
// Table-free for ROM economy; this only runs on settings writes, so
// the per-byte loop cost is irrelevant.
uint32_t iLabsFotaQspi::crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// CRC-16-CCITT, "XMODEM" parameterisation: poly 0x1021, init 0xFFFF,
// no input/output reflection, no final XOR. Matches Nordic SDK's
// crc16_compute and the host-side signing tool. Table-free; the only
// call site is the slot-header validate path, which runs once per OTA.
uint16_t iLabsFotaQspi::crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                   : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// Single source of truth for "is this block valid?" -- mirrors the
// rules in ilabs_fota_settings.h. Used by both readSettings (to pick
// the active block) and writeSettings (to figure out which sector to
// overwrite).
static bool settings_block_valid(const ilabs_fota_settings_t& s) {
    if (s.magic          != ILABS_FOTA_SETTINGS_MAGIC)   return false;
    if (s.struct_size    != ILABS_FOTA_SETTINGS_SIZE)    return false;
    if (s.struct_version != ILABS_FOTA_SETTINGS_VERSION) return false;
    uint32_t want = iLabsFotaQspi::crc32_ieee(
        reinterpret_cast<const uint8_t*>(&s), kSettingsCrcRange);
    return want == s.crc32;
}

bool iLabsFotaQspi::readSettings(ilabs_fota_settings_t* out) const {
    if (!_ready || out == nullptr) return false;

    ilabs_fota_settings_t a, b;
    bool a_ok = read_abs(kSettingsAAddr, (uint8_t*)&a, sizeof(a));
    bool b_ok = read_abs(kSettingsBAddr, (uint8_t*)&b, sizeof(b));
    if (!a_ok && !b_ok) return false;

    bool av = a_ok && settings_block_valid(a);
    bool bv = b_ok && settings_block_valid(b);

    if (av && bv) {
        // Pick higher sequence; ties (shouldn't happen) prefer A.
        *out = (a.sequence >= b.sequence) ? a : b;
        return true;
    }
    if (av) { *out = a; return true; }
    if (bv) { *out = b; return true; }
    return false;   // neither block valid -- caller treats as uninitialised
}

bool iLabsFotaQspi::writeSettings(const ilabs_fota_settings_t* in) {
    if (!_ready || in == nullptr) return false;

    // Read both blocks to decide which to overwrite + what sequence
    // number to use. The block to overwrite is the OLDER one (or
    // either, if neither is valid).
    ilabs_fota_settings_t a, b;
    bool a_present = read_abs(kSettingsAAddr, (uint8_t*)&a, sizeof(a));
    bool b_present = read_abs(kSettingsBAddr, (uint8_t*)&b, sizeof(b));
    bool a_ok = a_present && settings_block_valid(a);
    bool b_ok = b_present && settings_block_valid(b);

    uint32_t current_seq = 0;
    bool     write_to_a;
    if (a_ok && b_ok) {
        current_seq = (a.sequence > b.sequence) ? a.sequence : b.sequence;
        write_to_a  = (a.sequence < b.sequence);  // older one
    } else if (a_ok) {
        current_seq = a.sequence;
        write_to_a  = false;                       // B is dead, write fresh to B
    } else if (b_ok) {
        current_seq = b.sequence;
        write_to_a  = true;                        // A is dead, write fresh to A
    } else {
        current_seq = 0;
        write_to_a  = true;                        // both dead, start with A
    }

    // Compose the new block: copy caller's fields, then overwrite the
    // bookkeeping fields with their canonical values (so callers don't
    // have to populate them) and recompute CRC. Reserved bytes zeroed.
    ilabs_fota_settings_t fresh = *in;
    fresh.magic          = ILABS_FOTA_SETTINGS_MAGIC;
    fresh.struct_size    = ILABS_FOTA_SETTINGS_SIZE;
    fresh.struct_version = ILABS_FOTA_SETTINGS_VERSION;
    fresh.sequence       = current_seq + 1;
    fresh.reserved8      = 0;
    memset(fresh.reserved, 0, sizeof(fresh.reserved));
    fresh.crc32          = crc32_ieee(
        reinterpret_cast<const uint8_t*>(&fresh), kSettingsCrcRange);

    uint32_t target = write_to_a ? kSettingsAAddr : kSettingsBAddr;

    // Erase that sector, then write the struct.
    if (!erase_one_sector_abs(target)) return false;
    if (!write_abs(target, reinterpret_cast<const uint8_t*>(&fresh),
                   sizeof(fresh))) {
        return false;
    }
    return true;
}

// -------- high-level FOTA control --------

bool iLabsFotaQspi::triggerUpdate(uint32_t download_fw_version) {
    ilabs_fota_settings_t s;
    if (!readSettings(&s)) {
        // Virgin QSPI / both blocks corrupt -- start a fresh block.
        // writeSettings overwrites magic/struct_size/struct_version and
        // computes sequence + crc, so we only need to zero the rest.
        memset(&s, 0, sizeof(s));
    }

    s.flags                    |= ILABS_FOTA_FLAG_UPDATE_PENDING;
    s.download_slot_fw_version  = download_fw_version;
    s.swap_retry_count          = 0;
    s.last_status_code          = ILABS_FOTA_STATUS_OK;
    // current_app_fw_version, backup_slot_fw_version, boot_attempt_count
    // are preserved as-read so the bootloader's rollback / watchdog
    // logic still sees the right reference points.

    return writeSettings(&s);
}

