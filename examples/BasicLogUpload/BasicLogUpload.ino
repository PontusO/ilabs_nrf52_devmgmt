/*
 * BasicLogUpload -- minimal end-to-end wiring of the log-upload service.
 *
 * The library compresses the NEW portion of a log (whatever accumulated
 * since the last confirmed upload) into self-contained gzip members and
 * POSTs them to a server that simply appends the bytes to a per-device
 * file -- concatenated gzip members are themselves a valid .gz, so the
 * server file always decompresses as one continuous log. A reference
 * receiver lives in examples/server-php/index.php.
 *
 * Three injection points every integration needs:
 *   1. A log SOURCE: where the bytes live + the upload high-water mark.
 *      The library never owns your log store -- it reads and advances
 *      through these callbacks (a real product typically backs them with a
 *      LittleFS-on-QSPI rotating log; here a RAM buffer keeps it simple).
 *   2. An HTTPS POST transport + its per-request body cap.
 *   3. DevMgmt.uploadLog(url, source, result) from your connectivity
 *      task, inside an already-open network session.
 *
 * The transport below is SIMULATED (prints the POST, fabricates the
 * server's JSON reply) so this sketch runs to the OK path on a bare board.
 * Replace it with your modem's HTTPS POST -- for Würth Adrastea-I, the
 * iLabs_nrf52_adrastea library's lte_httpsPost() is a hardware-proven fit
 * (see the LteFotaAndLogUpload example for a real-modem wiring). Read the
 * DELIVERY CONTRACT comment there first; it is load-bearing.
 *
 * Unlike FOTA, log upload needs no QSPI staging and no bootloader --
 * DevMgmt.begin() is not required for uploadLog().
 */

#include <iLabs_nrf52_devmgmt.h>

// ---- 1. The log source: a demo RAM log --------------------------------
//
// Contract (see ilabs_fota_transport.h, ilabs_log_source_t):
//   read(offset, buf, len_inout)  fill buf from byte `offset`, may return
//                                 SHORT at internal boundaries; the
//                                 library loops until it has its take.
//   total()                       bytes existing in the log right now
//   uploaded()                    high-water: bytes confirmed uploaded
//   mark(consumed)                advance the high-water (persist it! --
//                                 a reboot must not re-send everything)
//   begin()/end()  (optional)     freeze a snapshot while uploading if
//                                 your store mutates/prunes concurrently
//                                 (a rotating-file log does); NULL here
//                                 because this demo buffer only appends.

static char     demoLog[8192];
static uint32_t demoLogLen      = 0;
static uint32_t demoLogUploaded = 0;   // in a real product: persist this

static void demoLogPrintln(const char* line) {
  size_t n = strlen(line);
  if (demoLogLen + n + 1 > sizeof(demoLog)) return;   // demo: just stop
  memcpy(demoLog + demoLogLen, line, n);
  demoLog[demoLogLen + n] = '\n';
  demoLogLen += n + 1;
}

static bool srcRead(uint32_t offset, uint8_t* buf, size_t* len_inout, void* u) {
  (void)u;
  if (offset >= demoLogLen) { *len_inout = 0; return false; }
  size_t avail = demoLogLen - offset;
  if (*len_inout > avail) *len_inout = avail;
  memcpy(buf, demoLog + offset, *len_inout);
  return true;
}
static uint32_t srcTotal(void* u)    { (void)u; return demoLogLen; }
static uint32_t srcUploaded(void* u) { (void)u; return demoLogUploaded; }
static void     srcMark(uint32_t consumed, void* u) {
  (void)u;
  demoLogUploaded = consumed;          // real product: also write to flash
}

static const ilabs_log_source_t demoSource = {
  srcRead, srcTotal, srcUploaded, srcMark,
  nullptr, nullptr,                    // begin/end snapshot hooks (optional)
  nullptr                              // user pointer
};

// ---- 2. HTTPS POST transport (SIMULATED -- replace with your modem) ----
//
// DELIVERY CONTRACT (load-bearing, see ilabs_fota_transport.h): return the
// HTTP status (>=200) when the server answered; return <= 0 ONLY if the
// request was DEFINITELY NOT delivered -- the library re-POSTs the same
// body on <= 0, so a transport that can "error" on a request the modem
// actually executed (the Adrastea AT%HTTPSEND does) must resolve the
// ambiguity itself (e.g. wait for the completion URC) before failing.
// Otherwise the server archive grows duplicate members.
//
// The cap passed to setUploadTransport() below is this transport's max
// body per request; the library sizes every gzip member to fit it.

#define POST_BODY_MAX 750   // e.g. Adrastea %HTTPSEND: 1500 PDU chars / 2

static int logPost(const char* url, const uint8_t* body, size_t body_len,
                   const char* sha256_hex, ilabs_fota_chunk_cb_t response_cb,
                   void* user) {
  (void)sha256_hex; (void)body;
  Serial.print("SIM POST ");
  Serial.print(body_len);
  Serial.print(" B (gzip member) -> ");
  Serial.println(url);

  // Fabricate the receiver's reply: {"received":N}. The library checks
  // received == body_len before advancing the high-water.
  char json[48];
  int n = snprintf(json, sizeof(json), "{\"received\":%u}", (unsigned)body_len);
  if (response_cb && n > 0) {
    response_cb((const uint8_t*)json, (size_t)n, (size_t)n, (size_t)n, user);
  }
  return 200;   // TODO: replace this whole function with your HTTPS POST
}

// ---- 3. Wire up + upload ------------------------------------------------

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  DevMgmt.setUploadTransport(logPost, POST_BODY_MAX);

  // Optional: route the library's own diagnostics somewhere visible.
  DevMgmt.setLogSink([](int level, const char* msg, void* u) {
    (void)u;
    static const char* const lv[] = { "DBG", "INF", "WRN", "ERR" };
    Serial.print('['); Serial.print(lv[level & 3]); Serial.print("] ");
    Serial.println(msg);
  });

  demoLogPrintln("[boot] BasicLogUpload demo started");
}

void loop() {
  // Generate some log traffic...
  static uint32_t cycle = 0;
  char line[64];
  snprintf(line, sizeof(line), "[cycle %lu] sensor=%.2f",
           (unsigned long)++cycle, 20.0 + (cycle % 10) * 0.5);
  demoLogPrintln(line);

  // ...and periodically ship whatever is new. A real product triggers
  // this from a downlink/schedule, inside its open modem session --
  // uploadLog() fires no session hooks and touches no QSPI.
  if (cycle % 20 == 0) {
    iLabsLogUploadResult r;
    if (DevMgmt.uploadLog("https://example.com/myproduct/logs/0011223344556677",
                          demoSource, r)) {
      Serial.print("upload OK: ");
      Serial.print(r.raw_bytes);
      Serial.print(" B raw -> ");
      Serial.print(r.compressed_bytes);
      Serial.println(" B gzip");
    } else {
      Serial.print("upload FAILED: status=");
      Serial.print((int)r.status);        // ilabs_log_upload_status_t
      Serial.print(" http=");
      Serial.println(r.http_status);
    }
  }

  delay(250);
}
