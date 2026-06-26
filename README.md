# iLabs nRF52 FOTA

Firmware-over-the-air updates for nRF52 devices with an external QSPI flash
and an Adafruit-fork bootloader.

The library owns the FOTA pipeline — download a gzip-compressed firmware
*slot* image over HTTPS, stream-inflate it, stage it into the QSPI download
partition, verify SHA-256 against the slot header, and arm the bootloader
settings block so the new image is applied on the next reboot. It is
**modem-agnostic**: the byte transport and every system coupling (logging,
sleep, radio coexistence) are injected, so the same library drops into any
nRF52 product regardless of how it reaches the network.

## Features

- Streaming gzip inflate during download — only a 32 KB window + small
  buffers in RAM, never the whole image.
- Just-in-time sector erase and a header-written-last commit, so an
  interrupted download leaves the slot in its erased state and the
  bootloader simply ignores it.
- Dual SHA-256: one over the compressed transport bytes (integrity), one
  over the decompressed payload (cross-checked against the slot header).
- Resumable, range-based megachunk download (default 100 KiB per request).
- Transactional A/B bootloader settings block with version + boot-confirm
  + rollback bookkeeping.
- A transport self-test mode (pattern verify, no flash write) for bringing
  up a new modem/transport.

## Requirements

- An nRF52 board with external QSPI flash (developed on the iLabs
  Connectivity 840 / W25Q64JV).
- The iLabs nRF52 Arduino BSP (provides `Adafruit_SPIFlashBase` + the nRFx
  QSPI HAL).
- A matching Adafruit-fork bootloader that understands the slot/settings
  contract in `src/ilabs_fota_settings.h` and `src/ilabs_fota_slot.h`.

## Installation

Copy this folder into your Arduino `libraries/` directory, or install the
release zip via *Sketch → Include Library → Add .ZIP Library*.

## Usage

```cpp
#include <iLabs_nrf52_fota.h>

// 1. Provide an HTTPS byte transport (forward to your modem driver).
static int rangeGet(const char* url, size_t off, size_t end,
                    ilabs_fota_chunk_cb_t cb, void* user) {
  return myHttpsGetWithRange(url, off, end, cb, user);   // 200/206 or <0
}

void setup() {
  FOTA.setTransport(rangeGet);
  FOTA.setFirmwareUrl("https://example.com/firmware.slot.gz");
  FOTA.setDeviceType(ILABS_DEVICE_TYPE);
  FOTA.begin();
  FOTA.confirmBoot(ILABS_FW_VERSION(1, 0, 0, 0));   // running image is healthy
}

void runUpdate() {
  iLabsFotaResult r;
  if (FOTA.update(r) && FOTA.triggerUpdate(r.header_fw_version)) {
    NVIC_SystemReset();   // bootloader applies the update on reboot
  }
}
```

See `examples/BasicFota` and `examples/TransportSelfTest`. The examples ship
with stub transports; a complete, hardware-proven Adrastea-I LTE transport
lives in the iLabs *pingday* firmware (`fota_transport_glue.cpp`, forwarding
to `lte_httpsGet` / `lte_httpsGetSocket`).

## Injection points

| What | How | Default |
|---|---|---|
| Ranged HTTPS GET (firmware) | `setTransport()` | none (required) |
| Plain HTTPS GET (self-test) | `setTestTransport()` | none |
| Log output | `setLogSink()` | dropped silently |
| Session begin/end | `onSessionBegin()` / `onSessionEnd()` | no-op |
| Firmware URL | `setFirmwareUrl()` | none |
| Device type | `setDeviceType()` | `ILABS_DEVICE_TYPE` (0x0052) |
| Megachunk size | `setMegachunkSize()` | 100 KiB |

The chunk-callback type `ilabs_fota_chunk_cb_t` is intentionally the same
shape as a typical streaming HTTP transport, so your adapter is a one-line
forward — and because it is type-checked, a transport signature change
surfaces as a compile error rather than a silent mismatch.

## QSPI partition map (fixed bootloader contract)

These addresses are a contract with the bootloader and are **not**
runtime-settable. Defaults target an 8 MB W25Q64JV:

```
0x000000 .. 0x5FFFFF   free / user region (6 MB)
0x600000 .. 0x6FFFFF   Slot B  — backup    (1 MB)
0x700000 .. 0x7FDFFF   Slot A  — download  (1 MB − 8 KB)
0x7FE000 .. 0x7FEFFF   Settings A
0x7FF000 .. 0x7FFFFF   Settings B
```

`src/ilabs_fota_settings.h` and `src/ilabs_fota_slot.h` are verbatim mirrors
of the bootloader-side headers (a three-way contract: bootloader, this
library, and the host-side signing tool). Keep all three in lockstep; the
compile-time `_Static_assert`s in each guard the struct layout.

## Bundled third-party code

`src/uzlib/` is [pfalcon/uzlib](https://github.com/pfalcon/uzlib) (zlib
license), vendored as the build's single copy of the DEFLATE codec
(inflate for FOTA, deflate available for callers such as log compression).

## License

MIT — see `LICENSE`. Bundled uzlib retains its own zlib license.
