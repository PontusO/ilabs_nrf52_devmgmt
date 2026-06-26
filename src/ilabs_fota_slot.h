/*
 * iLabs FOTA slot binary format.
 *
 * Three-way contract between:
 *   - This bootloader  (reads slots from QSPI, verifies, copies to app region)
 *   - The application  (writes downloaded firmware to the QSPI download slot)
 *   - The signing tool (host-side, emits slot files: header + payload [+ sig])
 *
 * The same layout is used for both the backup slot and the download slot, so
 * the bootloader can carry a single verify/copy code path for both partitions
 * (see doc/nrf52-fota-architecture.md).
 *
 * MIRROR NOTE
 * -----------
 * This file is a verbatim mirror of
 *   Adafruit_nRF52_Bootloader/src/ilabs_fota_slot.h
 * Any change to the struct, slot magic, slot-type values, or
 * ILABS_DEVICE_TYPE must be applied in BOTH places in the same change
 * set, plus mirrored to the host-side signing tool. The bootloader copy
 * is canonical; this copy must track it byte-for-byte.
 *
 * Phase 1 (unsigned) wire format:
 *
 *     [ ilabs_fota_slot_header_t (84 B) ][ payload (payload_size B) ]
 *
 *     header.flags        = 0
 *     header.sig_offset   = 0
 *     header.sig_size     = 0
 *     header.payload_sha256 = SHA-256(payload bytes)
 *
 * Phase 2 (signed) wire format -- additive, same header struct:
 *
 *     [ header (84 B) ][ payload ][ signature blob (sig_size B) ]
 *
 *     header.flags       |= ILABS_SLOT_FLAG_SIGNED
 *     header.sig_offset   = sizeof(header) + payload_size
 *     header.sig_size     = 64                              (ECDSA P-256 r||s)
 *
 * In phase 1 the verify path is just SHA-256 check; in phase 2 the
 * ECDSA-verify block (over header||payload) is compiled in additionally.
 * No format change between phases.
 */

#ifndef ILABS_FOTA_SLOT_H__
#define ILABS_FOTA_SLOT_H__

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
 * Slot magic: ASCII 'P','S','L','O' (iLabs Slot).
 *
 * Encoded so that on a little-endian nRF52 the bytes in flash read as
 * 'O','L','S','P' (LSB first). The 0x49534C4Fu constant matches the
 * MSB-first hex spelling -- same convention used by QSPI_BACKUP_MAGIC
 * in qspi_flash.h.
 */
#define ILABS_SLOT_MAGIC              0x49534C4Fu  /* 'ISLO' */

#define ILABS_SLOT_HEADER_VERSION     1u

/*
 * Device type. Must match the application's --dev-type at sign time. Mirrors
 * (and intentionally aliases) ADAFRUIT_DEVICE_TYPE from src/dfu_init.c:84 so
 * that BLE-DFU and LTE-FOTA packaging share the same device-identity check.
 */
#define ILABS_DEVICE_TYPE             0x0052u

/* Slot-type discriminator. Lives in the header so the bootloader can refuse
 * to apply a backup payload as if it were a download (and vice versa) even
 * if a buggy writer puts bytes at the wrong QSPI offset. */
#define ILABS_SLOT_TYPE_INVALID       0u
#define ILABS_SLOT_TYPE_BACKUP        1u
#define ILABS_SLOT_TYPE_DOWNLOAD      2u

/* Header flags. */
#define ILABS_SLOT_FLAG_SIGNED        (1u << 0)  /* signature blob present */
/* bits 1..31 reserved; MUST be written zero, MUST be ignored on read */

/* Signature algorithm constants (phase 2). */
#define ILABS_SIG_SIZE_ECDSA_P256     64u        /* raw r||s, 32+32 */

/*
 * Slot header. 84 bytes, all fields naturally aligned on a little-endian
 * 32-bit MCU; no compiler padding is required and the struct must not be
 * packed. The _Static_assert below catches any drift.
 *
 * Field rules:
 *   - All multi-byte integers are little-endian (native nRF52 + native x86_64
 *     host => no byte-swap in the signing tool).
 *   - `reserved` MUST be zero-filled by the writer and ignored by the reader.
 *   - `header_crc16` is computed last and covers the first
 *     (header_size - sizeof(header_crc16)) bytes.
 */
typedef struct
{
    uint32_t magic;              /* ILABS_SLOT_MAGIC */
    uint16_t header_size;        /* == sizeof(ilabs_fota_slot_header_t) */
    uint16_t header_version;     /* ILABS_SLOT_HEADER_VERSION */

    uint32_t payload_size;       /* bytes of payload immediately following header */
    uint8_t  payload_sha256[32]; /* SHA-256 over the payload bytes (in slot order) */

    uint32_t fw_version;         /* (major<<24)|(minor<<16)|(patch<<8)|rev -- monotonic */
    uint32_t device_type;        /* ILABS_DEVICE_TYPE; bootloader rejects mismatch */
    uint32_t slot_type;          /* ILABS_SLOT_TYPE_BACKUP | _DOWNLOAD */
    uint32_t flags;              /* ILABS_SLOT_FLAG_* */

    uint32_t sig_offset;         /* file-offset of signature blob from slot base; 0 if unsigned */
    uint32_t sig_size;           /* signature blob size in bytes; 0 if unsigned */

    uint8_t  reserved[14];       /* zero-fill; covered by header_crc16 */
    uint16_t header_crc16;       /* CRC-16-CCITT over preceding (header_size - 2) bytes */
} ilabs_fota_slot_header_t;

#define ILABS_SLOT_HEADER_SIZE        84u

/* Lock the layout down at build time so a compiler or struct-edit drift
 * can't silently break the three-way contract. */
_Static_assert(sizeof(ilabs_fota_slot_header_t) == ILABS_SLOT_HEADER_SIZE,
               "ilabs_fota_slot_header_t size drifted -- signing tool and "
               "app-side writer must be updated in lockstep.");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, magic)            == 0,  "magic offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, header_size)      == 4,  "header_size offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, header_version)   == 6,  "header_version offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, payload_size)     == 8,  "payload_size offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, payload_sha256)   == 12, "payload_sha256 offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, fw_version)       == 44, "fw_version offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, device_type)      == 48, "device_type offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, slot_type)        == 52, "slot_type offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, flags)            == 56, "flags offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, sig_offset)       == 60, "sig_offset offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, sig_size)         == 64, "sig_size offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, reserved)         == 68, "reserved offset");
_Static_assert(__builtin_offsetof(ilabs_fota_slot_header_t, header_crc16)     == 82, "header_crc16 offset");

/*
 * Build a fw_version word from its parts. major.minor.patch.rev each 0..255.
 * Comparing two fw_version uint32_t values with > / < gives correct
 * "newer than" ordering, which is what the rollback-prevention check uses.
 */
#define ILABS_FW_VERSION(major, minor, patch, rev) \
    ( ((uint32_t)(major) << 24) | ((uint32_t)(minor) << 16) | \
      ((uint32_t)(patch) <<  8) | ((uint32_t)(rev)         ) )

#ifdef __cplusplus
}
#endif

#endif /* ILABS_FOTA_SLOT_H__ */
