<?php
/*
 * iLabs log upload receiver -- server-side handler.
 *
 * Endpoint: POST https://ilabs.se/files/<product>/logs/{deveui}
 *
 * Place this file at:
 *   /web-root/files/<product>/logs/index.php
 *
 * And either:
 *   (a) put an .htaccess in the same directory that rewrites
 *       "/files/<product>/logs/{deveui}" -> "index.php" with
 *       the {deveui} captured in $_SERVER['REQUEST_URI'], OR
 *   (b) configure mod_rewrite at the vhost level.
 *
 * The handler:
 *   1. Extracts {deveui} from REQUEST_URI, validates it as 16 hex chars.
 *   2. Requires POST. Reads php://input.
 *   3. If X-Log-SHA256 header is present, validates against sha256($body).
 *      Mismatch -> 400 (lets the device retry the upload).
 *   4. Appends the gzip member into storage/{deveui}/{UTC-date}.log.gz
 *      -- gzip members concatenate, so the daily file decompresses as a
 *      whole (always-append; no dedup -- see the Store section).
 *   5. Returns 200 with JSON {"received": N, "stored_as": ...}.
 *
 * Storage retention: ad hoc -- run a cron that deletes files older
 * than 90 days. Example:
 *   find <storage_dir> -name "*.log.gz" -mtime +90 -delete
 *
 * Storage location: `__DIR__ . '/storage'` -- so the per-DevEUI
 * subdirectories live as siblings of this script inside the web root.
 * Pick this path because we don't need sudo to manage it AND it stays
 * within whatever sandbox the hosting environment enforces. The
 * companion .htaccess file inside storage/ blocks public GETs so the
 * uploads aren't browsable.
 *
 * The device firmware uses the JSON "received" field to decide whether
 * the POST succeeded end-to-end (received must match Content-Length).
 * Only then does Log.markUploaded() advance.
 */

// ----- Config ---------------------------------------------------------

$STORAGE_ROOT       = __DIR__ . '/storage';
$MAX_BODY_BYTES     = 256 * 1024;     // hard cap: refuse runaway uploads
$DEVEUI_REGEX       = '/^[0-9A-Fa-f]{16}$/';

// ----- Tiny helpers ---------------------------------------------------

function respond_json(int $status, array $body): void {
    http_response_code($status);
    $json = json_encode($body);
    header('Content-Type: application/json');
    // Emit an explicit Content-Length so the device's modem HTTP client
    // knows exactly when the body ends. Without it, PHP/Apache may use
    // chunked transfer-encoding (or Varnish may re-frame the response),
    // leaving the modem's receive loop waiting for a peer-close that
    // doesn't come promptly.
    //
    // We deliberately do NOT set Connection: close here. It's a
    // hop-by-hop header that PHP/userspace shouldn't manage -- Varnish
    // owns the client-facing connection -- and forcing a close appears
    // to make the Adrastea modem tear the socket down mid-RECEIVE,
    // hanging socketRecv. Content-Length is the clean signal.
    header('Content-Length: ' . strlen($json));
    echo $json;
    exit;
}

function get_header_ci(string $name): ?string {
    $key = 'HTTP_' . strtoupper(str_replace('-', '_', $name));
    return isset($_SERVER[$key]) ? $_SERVER[$key] : null;
}

// ----- Method check ---------------------------------------------------

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    respond_json(405, ['error' => 'method not allowed, use POST']);
}

// ----- Extract + validate {deveui} from URL ---------------------------

// REQUEST_URI looks like "/files/<product>/logs/70B3D57ED052599C".
// Trailing slash + query string optional.
$uri = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);
$segments = array_values(array_filter(explode('/', $uri), 'strlen'));
$deveui = end($segments);
if ($deveui === false || !preg_match($DEVEUI_REGEX, $deveui)) {
    respond_json(400, ['error' => 'missing or invalid {deveui} in URL']);
}
$deveui = strtoupper($deveui);

// ----- Read body ------------------------------------------------------

$content_length = isset($_SERVER['CONTENT_LENGTH'])
                ? (int)$_SERVER['CONTENT_LENGTH'] : 0;
if ($content_length > $MAX_BODY_BYTES) {
    respond_json(413, [
        'error'   => 'payload too large',
        'max'     => $MAX_BODY_BYTES,
        'claimed' => $content_length,
    ]);
}

$body = file_get_contents('php://input');
if ($body === false) {
    respond_json(500, ['error' => 'could not read request body']);
}
$body_len = strlen($body);
if ($body_len === 0) {
    respond_json(400, ['error' => 'empty body']);
}

// ----- Optional SHA-256 verification ----------------------------------

$sha_header = get_header_ci('X-Log-SHA256');
if ($sha_header !== null) {
    $sha_header = strtolower(trim($sha_header));
    if (!preg_match('/^[0-9a-f]{64}$/', $sha_header)) {
        respond_json(400, ['error' => 'X-Log-SHA256 not 64 hex chars']);
    }
    $computed = hash('sha256', $body);
    if (!hash_equals($computed, $sha_header)) {
        respond_json(400, [
            'error'    => 'sha256 mismatch -- request body corrupt in transit',
            'expected' => $sha_header,
            'got'      => $computed,
            'received' => $body_len,
        ]);
    }
}

// ----- Store (append into one rolling file per device) ----------------
//
// Each POST body is a complete, self-contained gzip member. Gzip members
// concatenate -- `cat a.gz b.gz | gunzip` yields both payloads -- so we
// append every member into one file per device per UTC day rather than
// writing a separate file per POST. The device fragments its log into
// many <=1500 B members (the modem's AT%HTTPSEND cap); the daily file
// reassembles them transparently and still decompresses as a whole.
//
// No dedup: the device's byte offset is relative to its oldest surviving
// log file and slides DOWN whenever a file is pruned, so it is not a
// monotonic key we could compare against a persisted high-water (an
// earlier ?seq scheme did exactly that and silently dropped every member
// after a device-side prune). We always append. A rare missed-200 re-send
// duplicates one member's worth of log lines, which is harmless for a
// diagnostic log and never corrupts the archive.

$dir = $STORAGE_ROOT . '/' . $deveui;
if (!is_dir($dir)) {
    if (!mkdir($dir, 0755, true)) {
        respond_json(500, ['error' => 'could not create storage dir']);
    }
}

$filename = gmdate('Y-m-d') . '.log.gz';
$path     = $dir . '/' . $filename;

if (file_put_contents($path, $body, FILE_APPEND | LOCK_EX) === false) {
    respond_json(500, ['error' => 'could not append log file']);
}

// Optional: record FW version header into a sidecar txt so the
// retention/analysis tooling can index uploads by firmware revision.
$fw_header = get_header_ci('X-Log-FW-Version');
if ($fw_header !== null) {
    @file_put_contents($path . '.fw',
                       preg_replace('/[^0-9A-Za-z.\-]/', '', $fw_header));
}

respond_json(200, [
    'received'  => $body_len,
    'stored_as' => $deveui . '/' . $filename,
]);
