/*
 * iLabs FOTA bootloader settings block.
 *
 * Persistent bootloader state used by the FOTA receive path. Lives in QSPI
 * flash, double-buffered across the last two 4 KB erase sectors so a power
 * failure mid-write leaves the previous generation intact.
 *
 * MIRROR NOTE
 * -----------
 * This file is a verbatim mirror of
 *   Adafruit_nRF52_Bootloader/src/ilabs_fota_settings.h
 * Any change to the struct, QSPI sector addresses, magic, flag bits, or
 * ILABS_FOTA_STATUS_* values must be applied in BOTH places in the same
 * change set. The bootloader copy is canonical; this copy must track it
 * byte-for-byte. The app is the writer in phase 1 (sets update_pending,
 * increments sequence, programs the inactive sector). The bootloader
 * becomes a writer too once the swap state machine + watchdog land
 * (tasks 5/6) -- at which point both sides write to the inactive block
 * under the same generation/CRC discipline.
 *
 * Wire format of one block (laid out at start of its 4 KB sector; the
 * remaining ~4032 bytes of the sector are unused / 0xFF after erase):
 *
 *     [ ilabs_fota_settings_t (64 B) ][ 0xFF padding to sector end ]
 *
 * Validation rules (must all hold for a block to be considered "valid"):
 *   - magic          == ILABS_FOTA_SETTINGS_MAGIC
 *   - struct_size    == ILABS_FOTA_SETTINGS_SIZE
 *   - struct_version == ILABS_FOTA_SETTINGS_VERSION
 *   - crc32          == crc32(block, struct_size - 4)
 *
 * Active block: the valid block with the higher sequence number.
 */

#ifndef ILABS_FOTA_SETTINGS_H__
#define ILABS_FOTA_SETTINGS_H__

#include <stdbool.h>
#include <stdint.h>

/* _Static_assert is a C11 keyword; in C++ the spelling is static_assert.
 * Map it so this bootloader-contract header compiles unchanged whether
 * included by the C bootloader or the C++ application/library. No-op in C,
 * so the bootloader-side copy can carry this block verbatim too. */
#if defined(__cplusplus) && !defined(_Static_assert)
#define _Static_assert static_assert
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * QSPI partition addresses. Single source of truth; the app-side FotaQspi
 * writer and the host-side signing tool must use these same values.
 *
 * Layout on the 8 MB W25Q64JV:
 *
 *   0x000000 .. 0x5FFFFF  free (6 MB, future use)
 *   0x600000 .. 0x6FFFFF  Slot B  -- backup slot (1 MB)
 *   0x700000 .. 0x7FDFFF  Slot A  -- download slot (1 MB - 8 KB)
 *   0x7FE000 .. 0x7FEFFF  Settings A
 *   0x7FF000 .. 0x7FFFFF  Settings B
 *
 * The two slots use ilabs_fota_slot_header_t (see ilabs_fota_slot.h);
 * the two settings sectors use ilabs_fota_settings_t (this header).
 */
#define ILABS_FOTA_QSPI_SIZE                  0x00800000u  /* 8 MB W25Q64JV */
#define ILABS_FOTA_SLOT_SIZE                  0x00100000u  /* 1 MB nominal */
#define ILABS_FOTA_DOWNLOAD_SLOT_ADDR         0x00700000u
#define ILABS_FOTA_DOWNLOAD_SLOT_USABLE       (ILABS_FOTA_SLOT_SIZE - 0x2000u) /* minus the two settings sectors */
#define ILABS_FOTA_BACKUP_SLOT_ADDR           0x00600000u
#define ILABS_FOTA_BACKUP_SLOT_USABLE         ILABS_FOTA_SLOT_SIZE
#define ILABS_FOTA_SETTINGS_A_ADDR            0x007FE000u  /* last-1 4K sector */
#define ILABS_FOTA_SETTINGS_B_ADDR            0x007FF000u  /* last 4K sector */
#define ILABS_FOTA_SETTINGS_SECTOR_SIZE       0x00001000u  /* 4 KB */

/*
 * Magic 'ISST' (iLabs SettingS). MSB-first hex spelling matches ASCII --
 * same convention as ILABS_SLOT_MAGIC ('ISLO') and the existing
 * QSPI_BACKUP_MAGIC ('BAKP') in qspi_flash.h. In little-endian flash the
 * bytes read 'T','S','S','P'.
 */
#define ILABS_FOTA_SETTINGS_MAGIC             0x49535354u  /* 'ISST' */
#define ILABS_FOTA_SETTINGS_VERSION           1u

/*
 * Flag bits in ilabs_fota_settings_t.flags. Bits not listed here MUST be
 * written zero and MUST be ignored on read -- that lets future bootloaders
 * add bits without breaking older app-side writers.
 */
#define ILABS_FOTA_FLAG_UPDATE_PENDING            (1u << 0)  /* download slot has a fresh image to apply */
#define ILABS_FOTA_FLAG_BOOT_CONFIRM_REQUIRED     (1u << 1)  /* watchdog: app must clear boot_attempt_count */

/*
 * Status codes -- written by the bootloader after a swap attempt so the app
 * can read on next boot and report upstream over LTE. Values 0..255.
 */
#define ILABS_FOTA_STATUS_OK                      0u
#define ILABS_FOTA_STATUS_VERIFY_FAILED           1u  /* SHA mismatch or signature fail */
#define ILABS_FOTA_STATUS_COPY_FAILED             2u  /* QSPI->internal copy errored or post-copy verify failed */
#define ILABS_FOTA_STATUS_VERSION_ROLLBACK        3u  /* download fw_version < current_app_fw_version */
#define ILABS_FOTA_STATUS_RETRY_EXHAUSTED         4u  /* swap_retry_count hit the max */
#define ILABS_FOTA_STATUS_DEVICE_TYPE_MISMATCH    5u  /* slot device_type != ILABS_DEVICE_TYPE */
#define ILABS_FOTA_STATUS_HEADER_INVALID          6u  /* slot magic/CRC16/struct_version/size mismatch */
#define ILABS_FOTA_STATUS_QSPI_ERROR              7u  /* QSPI read/erase/program returned failure */
#define ILABS_FOTA_STATUS_ROLLED_BACK             8u  /* watchdog restored backup after MAX failed boot attempts */
/* values 9..255 reserved for future codes */

/*
 * Settings block layout. 64 bytes, all fields naturally aligned on a
 * little-endian 32-bit MCU; no compiler padding is required and the struct
 * must not be packed. The _Static_asserts below catch any drift.
 *
 * All multi-byte integers are little-endian (native nRF52 + native x86_64).
 * `reserved` MUST be zero-filled by the writer and ignored by the reader.
 * `crc32` covers the first (struct_size - sizeof(crc32)) bytes.
 */
typedef struct
{
    uint32_t magic;                      /* ILABS_FOTA_SETTINGS_MAGIC */
    uint16_t struct_size;                /* == ILABS_FOTA_SETTINGS_SIZE */
    uint16_t struct_version;             /* ILABS_FOTA_SETTINGS_VERSION */

    uint32_t sequence;                   /* monotonic; higher = active block */
    uint32_t flags;                      /* ILABS_FOTA_FLAG_* */

    uint32_t download_slot_fw_version;   /* 0 if no valid download */
    uint32_t backup_slot_fw_version;     /* 0 if no valid backup */
    uint32_t current_app_fw_version;     /* last successfully booted app version */

    uint8_t  swap_retry_count;           /* bumped before each swap attempt, cleared on success */
    uint8_t  boot_attempt_count;         /* bumped by bootloader, cleared by confirmed-good app */
    uint8_t  last_status_code;           /* ILABS_FOTA_STATUS_* -- explains prior swap outcome */
    uint8_t  reserved8;                  /* zero-fill */

    uint8_t  reserved[28];               /* zero-fill; covered by crc32 */
    uint32_t crc32;                      /* CRC-32/IEEE over preceding (struct_size - 4) bytes */
} ilabs_fota_settings_t;

#define ILABS_FOTA_SETTINGS_SIZE              64u

/* Lock the layout down at build time so a compiler or struct edit can't
 * silently break the bootloader/app contract. */
_Static_assert(sizeof(ilabs_fota_settings_t) == ILABS_FOTA_SETTINGS_SIZE,
               "ilabs_fota_settings_t size drifted - app-side writer "
               "(FotaQspi.{h,cpp}) must be updated in lockstep.");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, magic)                    == 0,  "magic offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, struct_size)              == 4,  "struct_size offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, struct_version)           == 6,  "struct_version offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, sequence)                 == 8,  "sequence offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, flags)                    == 12, "flags offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, download_slot_fw_version) == 16, "download_slot_fw_version offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, backup_slot_fw_version)   == 20, "backup_slot_fw_version offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, current_app_fw_version)   == 24, "current_app_fw_version offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, swap_retry_count)         == 28, "swap_retry_count offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, boot_attempt_count)       == 29, "boot_attempt_count offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, last_status_code)         == 30, "last_status_code offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, reserved8)                == 31, "reserved8 offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, reserved)                 == 32, "reserved offset");
_Static_assert(__builtin_offsetof(ilabs_fota_settings_t, crc32)                    == 60, "crc32 offset");

#ifdef __cplusplus
}
#endif

#endif /* ILABS_FOTA_SETTINGS_H__ */
