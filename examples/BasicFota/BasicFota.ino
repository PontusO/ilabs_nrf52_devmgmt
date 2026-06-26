/*
 * BasicFota -- minimal end-to-end wiring of the iLabs nRF52 FOTA library.
 *
 * Four steps every integration needs:
 *   1. Provide an HTTPS byte transport (the library is modem-agnostic).
 *   2. (Optional) provide a log sink + session hooks.
 *   3. Configure the firmware URL and device type.
 *   4. Call FOTA.update(); on success, FOTA.triggerUpdate() + reboot.
 *
 * The transport adapter below is a STUB that returns -1 ("no transport").
 * In a real product you forward it to your modem driver -- the pingday
 * firmware forwards this to its Adrastea-I LTE driver's lte_httpsGet().
 * See that sketch for a complete, hardware-proven transport. With the
 * stub, this sketch compiles and runs but every update fails fast.
 */

#include <iLabs_nrf52_fota.h>

// ---- 1. HTTPS transport: closed-range GET (replace body w/ your modem) ----
static int fotaRangeGet(const char* url, size_t off, size_t end,
                        ilabs_fota_chunk_cb_t cb, void* user) {
  // Request bytes [off..end] inclusive; deliver each decoded chunk to cb().
  // Return the HTTP status (200/206) or a negative on transport error.
  (void)url; (void)off; (void)end; (void)cb; (void)user;
  return -1;   // TODO: forward to your HTTPS-GET-with-Range implementation
}

// ---- 2. Optional log sink (0=DBG 1=INF 2=WRN 3=ERR) ----
static void fotaLog(int level, const char* msg, void* user) {
  (void)user;
  static const char* const lv[] = { "DBG", "INF", "WRN", "ERR" };
  Serial.print('['); Serial.print(lv[level & 3]); Serial.print("] ");
  Serial.println(msg);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  FOTA.setLogSink(fotaLog);
  FOTA.setTransport(fotaRangeGet);

  FOTA.setFirmwareUrl("https://example.com/connectivity840/firmware.slot.gz");
  FOTA.setDeviceType(ILABS_DEVICE_TYPE);    // 0x0052 for Connectivity 840

  if (!FOTA.begin()) {
    Serial.println("FOTA.begin() failed -- QSPI flash not detected");
    return;
  }

  // Tell the bootloader the running image is healthy so it won't roll
  // back. Call once you've reached a known-good milestone.
  FOTA.confirmBoot(ILABS_FW_VERSION(1, 0, 0, 0));
}

void loop() {
  // A real product triggers this from a button, a downlink, or a schedule
  // -- not every loop. Shown inline here for brevity.
  iLabsFotaResult r;
  if (FOTA.update(r)) {
    Serial.println("FOTA OK -- arming bootloader and rebooting");
    if (FOTA.triggerUpdate(r.header_fw_version)) {
      delay(1000);
      NVIC_SystemReset();     // bootloader applies UPDATE_PENDING on reboot
    }
  } else {
    Serial.print("FOTA not applied (http=");
    Serial.print(r.http_status);
    Serial.print(", committed=");
    Serial.print(r.slot_committed);
    Serial.println(")");
  }

  FOTA.suspend();             // QSPI -> deep-power-down between attempts
  delay(60UL * 1000UL);
  FOTA.resume();
}
