/*
 * LteFotaAndLogUpload -- a REAL end-to-end example driving both iLabs
 * libraries over a live Würth Adrastea-I LTE modem on the Connectivity 840
 * (nRF52840) board:
 *
 *   iLabs_nrf52_adrastea  -- the modem transport: AT driver, power
 *                            sequencing, attach/PDP primitives, HTTPS
 *                            GET (ranged) + POST, CA provisioning.
 *   iLabs_nrf52_devmgmt   -- FOTA (download+stage+verify a slot image) and
 *                            transport-agnostic log upload.
 *
 * The flow, once per cycle:
 *   1. Power the modem, wait for the PowerManager ">>"/map + LTE-ready URC.
 *   2. lte_attach() + lte_activatePDP() -> data plane up.
 *   3. Provision the Let's Encrypt root CA into modem NV (once).
 *   4. Trigger a FOTA against a DUMMY firmware URL/image name.
 *   5. Trigger a log upload against a DUMMY logs URL, from a demo RAM log.
 *   6. Power the modem down and wait for the next cycle.
 *
 * The URLs and firmware image name below are DUMMY placeholders -- point
 * them at a real endpoint (see examples/server-php) to see the transfers
 * complete; as-is they exercise the whole path and fail cleanly at the
 * fetch/POST. You DO need a provisioned SIM (set the APN) for the modem to
 * attach. Requires a W25Q64-class QSPI flash + the Adafruit-fork bootloader
 * for the FOTA staging half (DevMgmt.begin()); the log-upload half needs
 * neither.
 */

#include <iLabs_nrf52_adrastea.h>   // modem transport
#include <iLabs_nrf52_devmgmt.h>    // FOTA + log upload

// ---- configuration (dummy where noted) ---------------------------------

#define LTE_APN              "iot.1nce.net"   // <-- set to YOUR SIM's APN
// DUMMY firmware URL (the last path element is the "image name"):
#define FOTA_FIRMWARE_URL    "https://example.com/connectivity840/fota/firmware.slot.gz"
// DUMMY per-device log-upload URL (…/logs/<16-hex-DevEUI>):
#define LOG_UPLOAD_URL       "https://example.com/myproduct/logs/0011223344556677"
// DUMMY per-device diagnostics URL (…/diag/<16-hex-DevEUI>):
#define DIAG_URL             "https://example.com/myproduct/diag/0011223344556677"

#define ATTACH_BUDGET_MS     (5UL * 60UL * 1000UL)   // first cold attach can be slow
#define CYCLE_PERIOD_MS      (10UL * 60UL * 1000UL)  // one FOTA+upload attempt / 10 min

// ---- 1. log sink: route both libraries' diagnostics to Serial ----------

static void logSink(int level, const char* msg, void* user) {
  (void)user;
  static const char* const lv[] = { "DBG", "INF", "WRN", "ERR" };
  Serial.print('['); Serial.print(lv[level & 3]); Serial.print("] ");
  Serial.println(msg);
}

// ---- 2. URC routing: feed modem URCs to every state machine ------------
//
// The AT driver's RX task calls this for each unsolicited line. Forward it
// to the network state machine (+CEREG for attach) and the HTTP/socket
// event handlers (%HTTPEVU / %SOCKETEV for GET/POST completion).

static void urcHandler(const char* line) {
  lte_processURC(line);          // +CEREG registration state
  lte_http_processURC(line);     // %HTTPEVU (HTTPCMD/HTTPSEND)
  lte_https_processURC(line);    // %SOCKETEV (socket path)
}

// ---- 3. transport adapters: adrastea -> devmgmt function-pointer types --
//
// The callback types line up exactly, so these are plain forwards.

static int fotaRangeGet(const char* url, size_t off, size_t end,
                        ilabs_fota_chunk_cb_t cb, void* user) {
  return lte_httpsGet(url, off, end, cb, user);            // ranged AT%HTTPCMD GET
}

static int logPost(const char* url, const uint8_t* body, size_t body_len,
                   const char* sha256_hex,
                   ilabs_fota_chunk_cb_t response_cb, void* user) {
  return lte_httpsPost(url, body, body_len, sha256_hex,
                       nullptr, response_cb, user);          // AT%HTTPSEND POST
}

// ---- 4. a demo RAM log source (see BasicLogUpload for the contract) ----

static char     demoLog[4096];
static uint32_t demoLogLen      = 0;
static uint32_t demoLogUploaded = 0;   // real product: persist across reboots

static void demoLogPrintln(const char* line) {
  size_t n = strlen(line);
  if (demoLogLen + n + 1 > sizeof(demoLog)) demoLogLen = 0;   // demo: wrap
  memcpy(demoLog + demoLogLen, line, n);
  demoLog[demoLogLen + n] = '\n';
  demoLogLen += n + 1;
}
static bool srcRead(uint32_t off, uint8_t* buf, size_t* len, void* u) {
  (void)u;
  if (off >= demoLogLen) { *len = 0; return false; }
  size_t avail = demoLogLen - off;
  if (*len > avail) *len = avail;
  memcpy(buf, demoLog + off, *len);
  return true;
}
static uint32_t srcTotal(void* u)    { (void)u; return demoLogLen; }
static uint32_t srcUploaded(void* u) { (void)u; return demoLogUploaded; }
static void     srcMark(uint32_t c, void* u) { (void)u; demoLogUploaded = c; }

static const ilabs_log_source_t demoSource = {
  srcRead, srcTotal, srcUploaded, srcMark, nullptr, nullptr, nullptr
};

// ---- modem bring-up: power on -> attach -> PDP -> CA -------------------

static bool bringUpModem() {
  modem_powerOn();
  if (!atModem_waitReady(20000)) {
    Serial.println("modem did not come ready");
    modem_powerOff();
    atModem_notifyPoweredOff();
    return false;
  }
  atModem_send("ATE0",      nullptr, 0, 3000);
  atModem_send("AT+CMEE=1", nullptr, 0, 3000);
  lte_logIdentity();

  if (!atModem_waitLteReady(60000)) {
    Serial.println("LTE stack not ready");
    return false;
  }
  if (!lte_attach(LTE_APN, ATTACH_BUDGET_MS)) {
    Serial.println("attach failed");
    return false;
  }
  if (!lte_activatePDP()) {
    Serial.println("PDP activation failed");
    return false;
  }
  // CA persists in modem NV; provisioning once per boot is enough. Cheap to
  // re-assert, so we just do it each bring-up in this example.
  if (!lte_httpProvisionLetsEncrypt()) {
    Serial.println("CA provisioning failed");
    return false;
  }
  return true;
}

static void powerDownModem() {
  lte_detach();                   // AT+CFUN=0 + clear cached state
  modem_powerOff();
  atModem_notifyPoweredOff();
}

// ---- setup -------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  Serial.println("LteFotaAndLogUpload example");

  // Transport library: logging + debug console + modem driver.
  adrastea_setLogSink(logSink);
  adrastea_setDebugStream(&Serial);   // enables atModem_enterPassthrough()
  modem_powerInit();                  // GPIO -> modem OFF
  atModem_init();                     // spawn the AT RX task
  atModem_setURCHandler(urcHandler);

  // Device-management library: logging + transports + firmware URL + QSPI.
  DevMgmt.setLogSink(logSink);
  DevMgmt.setTransport(fotaRangeGet);                     // FOTA ranged GET
  DevMgmt.setUploadTransport(logPost, LTE_HTTP_POST_BODY_MAX); // log POST
  DevMgmt.setFirmwareUrl(FOTA_FIRMWARE_URL);
  DevMgmt.setDeviceType(ILABS_DEVICE_TYPE);              // 0x0052 = Connectivity 840

  if (!DevMgmt.begin()) {
    // FOTA staging needs the QSPI flash; the log-upload half still works.
    Serial.println("DevMgmt.begin() failed -- QSPI not detected (FOTA disabled)");
  }

  demoLogPrintln("[boot] LteFotaAndLogUpload started");
}

// ---- loop: one FOTA + log-upload attempt per cycle ---------------------

void loop() {
  // Generate a little demo log traffic to upload.
  static uint32_t n = 0;
  char line[64];
  snprintf(line, sizeof(line), "[cycle %lu] uptime=%lus", (unsigned long)++n,
           (unsigned long)(millis() / 1000));
  demoLogPrintln(line);

  Serial.println("=== bringing modem up ===");
  if (bringUpModem()) {
    // --- FOTA against the DUMMY firmware URL/image name ---
    Serial.println("=== FOTA attempt ===");
    if (DevMgmt.ready()) {
      DevMgmt.resume();                 // wake QSPI staging
      iLabsFotaResult fr;
      bool ok = DevMgmt.update(fr);     // uses FOTA_FIRMWARE_URL
      Serial.print("FOTA: committed="); Serial.print((int)fr.slot_committed);
      Serial.print(" http=");           Serial.println(fr.http_status);
      if (ok && fr.slot_committed && DevMgmt.triggerUpdate(fr.header_fw_version)) {
        Serial.println("FOTA staged -- rebooting");
        delay(1000);
        NVIC_SystemReset();
      }
      DevMgmt.suspend();
    }

    // --- log upload against the DUMMY logs URL ---
    Serial.println("=== log upload attempt ===");
    iLabsLogUploadResult lr;
    bool ok = DevMgmt.uploadLog(LOG_UPLOAD_URL, demoSource, lr);
    Serial.print("upload: ok="); Serial.print((int)ok);
    Serial.print(" status=");    Serial.print((int)lr.status);
    Serial.print(" raw=");       Serial.print(lr.raw_bytes);
    Serial.print(" gzip=");      Serial.println(lr.compressed_bytes);

    // --- a diagnostic against the DUMMY diag URL ---
    // One short severity-tagged line, server-timestamped, shown on the device
    // page. ILABS_DIAG_DIRECT here just POSTs within this already-open modem
    // session (this example owns the session, so no begin/end hooks are
    // registered and the mode's open/close is a no-op). A product that lets the
    // library drive the link would register onSessionBegin/onSessionEnd and use
    // ILABS_DIAG_KEEP_OPEN / ILABS_DIAG_CLOSE to batch several sends per link.
    Serial.println("=== diagnostic attempt ===");
    int ds = DevMgmt.postDiagnostic(DIAG_URL, 1 /*INFO*/, line,
                                    ILABS_DIAG_DIRECT);
    Serial.print("diag: http="); Serial.println(ds);
  }
  powerDownModem();

  Serial.print("=== cycle done; sleeping ");
  Serial.print(CYCLE_PERIOD_MS / 1000);
  Serial.println(" s ===");
  delay(CYCLE_PERIOD_MS);
}
