// Runtime paint check for the wasm app modules -- no Playwright, pure W3C
// WebDriver over Node's stdlib.
//
// The node smoke (wasm-node-smoke.mjs) only compile-validates the module; an app
// that links but aborts at runtime (missing font, toolkit-init trap) or comes up
// as a 0x0 canvas slips through. This drives each app in a real WebKit browser
// through the standard WebDriver protocol and asserts its canvas reaches a
// nonzero size and paints non-blank pixels.
//
// Driver, autodetected (override with WASM_PAINT_WEBDRIVER=/path/to/driver):
//   - macOS: `safaridriver` -> real Safari. Needs a one-time
//     `safaridriver --enable` (Allow Remote Automation in Safari's Develop
//     settings); it drives real Safari windows (not headless).
//   - Linux/CI: `WebKitWebDriver` from the distro `webkit2gtk-driver` package,
//     run under xvfb (WebKitGTK has no headless mode). Same engine family as
//     Safari, ABI-matched to the runner's system libraries.
// The single W3C client below talks to either. Headless WebKit has no WebGL, so
// the harness forces the compat software present path (same drawing code, only
// the final blit differs).
//
// Usage: node scripts/wasm-paint-check.mjs <build-dir> <app> [more-apps ...]
//        <app> is a basename without extension (clock, xnedit, ...); its
//        <app>.js and, if present, <app>.data must sit in build-dir.
// Env:   WASM_PAINT_TIMEOUT_MS (default 20000), WASM_PAINT_CALIBRATE=1 (report
//        only, never fail), WASM_PAINT_WEBDRIVER (driver binary path).

import { createServer } from "node:http";
import { readFile, access } from "node:fs/promises";
import { extname, join, resolve, sep } from "node:path";
import { spawn } from "node:child_process";
import { request } from "node:http";
import { platform } from "node:os";

const [buildDir, ...appArgs] = process.argv.slice(2);
if (!buildDir || appArgs.length === 0) {
  console.error("wasm-paint-check: usage: <build-dir> <app> [more-apps ...]");
  process.exit(2);
}
const apps = appArgs.map((a) => a.replace(/\.(html|js)$/, ""));
const TIMEOUT_MS = Number(process.env.WASM_PAINT_TIMEOUT_MS || 20000);
const CALIBRATE = process.env.WASM_PAINT_CALIBRATE === "1";
// Locally a missing WebKit driver is a clean skip; in CI (which installs one)
// it must be a hard failure, or the paint gate silently passes with no browser.
const REQUIRED = process.env.WASM_PAINT_REQUIRED === "1";
function skipOrFail(msg) {
  if (REQUIRED) { console.error(`  FAIL    wasm-paint-check (${msg}; WASM_PAINT_REQUIRED=1)`); return 1; }
  console.error(`  SKIP    wasm-paint-check (${msg})`);
  return 0;
}

const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm",
  ".data": "application/octet-stream", ".mem": "application/octet-stream",
};

// Minimal harness served at the build root so the app's relative <app>.js /
// .wasm / .data resolve normally; it forces the software present path.
const harness = (name) => `<!doctype html><html><head><meta charset="utf-8">
</head><body><canvas id="canvas" oncontextmenu="event.preventDefault()" tabindex="-1"></canvas>
<script>
// Capture the first failure cause from every channel: a wasm trap surfaces as an
// unhandledrejection, a loader/JS failure as window.onerror or the script tag's
// onerror, and a Module abort as printErr. Without these a trap reads as a bare
// 0x0 canvas with no reason.
// Keep the first cause and the latest one: an early benign printErr must not
// mask the fatal trap that actually explains the blank canvas.
function _cap(t){ var s=String(t); if(window.__err===undefined) window.__err=s; window.__errLast=s; }
window.addEventListener("error", function(e){ _cap((e && e.message) || e); });
window.addEventListener("unhandledrejection", function(e){
  _cap((e && e.reason && (e.reason.message || e.reason)) || "unhandledrejection"); });
var Module = {
  canvas: document.getElementById("canvas"),
  thisProgram: ${JSON.stringify(name === "xwpe" ? "xwpe" : undefined)},
  preRun: [function(){ try {
    ENV.LIBX11_COMPAT_FORCE_SOFTWARE_PRESENT = "1";
    if (${JSON.stringify(name)} === "xwpe") {
      ENV.XWPE_LIB = "/usr/local/lib/xwpe";
    }
  } catch(e){} }],
  print: function(){}, printErr: function(t){ _cap(t); }
};
</script><script src="${name}.js" onerror="_cap('failed to load ${name}.js')"></script></body></html>`;

const server = createServer(async (req, res) => {
  const rel = decodeURIComponent(req.url.split("?")[0]).replace(/^\/+/, "");
  const m = rel.match(/^(.+)\.__paint__\.html$/);
  if (m) {
    res.writeHead(200, { "Content-Type": "text/html" });
    res.end(harness(m[1]));
    return;
  }
  // Reject paths that escape the build dir. A ".." or absolute-path request
  // (from a compromised or generated app script) must not read arbitrary files
  // from the checkout while the harness is running.
  const full = resolve(buildDir, rel);
  const root = resolve(buildDir);
  if (full !== root && !full.startsWith(root + sep)) {
    res.writeHead(403);
    res.end("forbidden");
    return;
  }
  try {
    const body = await readFile(full);
    res.writeHead(200, { "Content-Type": MIME[extname(rel)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404);
    res.end("not found");
  }
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const httpPort = server.address().port;

// The probe runs in the page via execute/sync and returns its value directly:
// backing size plus a non-blank signal (the whole canvas downscaled onto a 2D
// scratch canvas, counting pixels differing from the top-left one).
const PROBE = `
  var c = document.getElementById("canvas");
  if (!c) return { found: false };
  var w = c.width, h = c.height, nonFirstPixels = -1;
  if (w > 0 && h > 0) {
    try {
      var sw = Math.min(w, 96), sh = Math.min(h, 96);
      var off = document.createElement("canvas");
      off.width = sw; off.height = sh;
      var ctx = off.getContext("2d");
      ctx.drawImage(c, 0, 0, sw, sh);
      var d = ctx.getImageData(0, 0, sw, sh).data;
      var first = d[0] + "," + d[1] + "," + d[2] + "," + d[3];
      nonFirstPixels = 0;
      for (var i = 4; i < d.length; i += 4) {
        if (d[i] + "," + d[i+1] + "," + d[i+2] + "," + d[i+3] !== first) nonFirstPixels++;
      }
    } catch (e) { nonFirstPixels = -2; }
  }
  return { found: true, w: w, h: h, nonFirstPixels: nonFirstPixels, err: window.__errLast || window.__err || null };
`;

// ---- driver selection ----
const isMac = platform() === "darwin";
const driverBin = process.env.WASM_PAINT_WEBDRIVER ||
  (isMac ? "safaridriver" : "WebKitWebDriver");
// safaridriver matches browserName "safari". WebKitGTK's WebDriver launches
// MiniBrowser by default and advertises browserName "MiniBrowser"; the exact
// string is version/package-specific, so allow an override
// (WASM_PAINT_BROWSERNAME) to retarget CI without a code change.
const browserName = process.env.WASM_PAINT_BROWSERNAME ||
  (isMac ? "safari" : "MiniBrowser");
const caps = isMac
  ? { alwaysMatch: { browserName } }
  : { alwaysMatch: { browserName, "webkitgtk:browserOptions": { args: ["--automation"] } } };

// ---- tiny W3C WebDriver client over node http ----
// Grab a real free port the same way as the http server above; a modulo of the
// http port can land on a live listener and turn into a flaky hard failure.
// There is a tiny TOCTOU between the close and the driver bind, negligible for
// a short-lived CI helper.
const drvPort = await (async () => {
  const s = createServer();
  await new Promise((r) => s.listen(0, "127.0.0.1", r));
  const p = s.address().port;
  await new Promise((r) => s.close(r));
  return p;
})();
function drvReq(method, path, body) {
  return new Promise((resolve, reject) => {
    const data = body === undefined ? "" : JSON.stringify(body);
    const req = request({ host: "127.0.0.1", port: drvPort, path, method,
      headers: { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(data) } },
      (res) => {
        let buf = "";
        res.on("data", (c) => (buf += c));
        res.on("end", () => {
          let json = null; try { json = JSON.parse(buf); } catch {}
          resolve({ status: res.statusCode, json });
        });
      });
    // A driver that accepts the socket then hangs must not stall the whole run.
    req.setTimeout(5000, () => req.destroy(new Error("driver request timeout")));
    req.on("error", reject);
    req.end(data);
  });
}
async function waitForDriver(deadline) {
  while (Date.now() < deadline) {
    try {
      const r = await drvReq("GET", "/status");
      if (r.status === 200 && r.json) return true;
    } catch {}
    await sleep(200);
  }
  return false;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// safaridriver takes `-p PORT` and binds appropriately; WebKitWebDriver takes
// `--host`/`--port` and otherwise binds ::1 (IPv6 localhost), which fails on an
// IPv6-disabled host and would not match this client's 127.0.0.1 anyway. Pin it
// to IPv4 so the client and driver agree.
const driverArgs = isMac
  ? ["-p", String(drvPort)]
  : [`--host=127.0.0.1`, `--port=${drvPort}`];
let driver;
try {
  driver = spawn(driverBin, driverArgs, { stdio: ["ignore", "ignore", "pipe"] });
} catch (e) {
  server.close();
  process.exit(skipOrFail(`${driverBin} not launchable: ${e.message}`));
}
let driverStderr = "";
driver.stderr.on("data", (c) => (driverStderr += c));
driver.on("error", () => {});

// The driver is a separate process (and launches a browser child); make sure it
// dies with us on an uncaught throw or Ctrl-C, not just the normal exit path
// below. The exit handler covers a thrown error; the signals do not fire exit
// on their own, so route them through process.exit.
process.on("exit", () => { try { driver.kill(); } catch {} });
process.on("SIGINT", () => process.exit(130));
process.on("SIGTERM", () => process.exit(143));

const ready = await waitForDriver(Date.now() + 8000);
if (!ready) {
  try { driver.kill(); } catch {}
  server.close();
  process.exit(skipOrFail(`${driverBin} did not start; is it installed?${driverStderr ? " stderr: " + driverStderr.slice(0, 200) : ""}`));
}

let failed = 0;
for (const name of apps) {
  try {
    await access(join(buildDir, `${name}.js`));
  } catch {
    console.error(`  FAIL    ${name}: ${name}.js not found in ${buildDir}`);
    failed++;
    continue;
  }

  let sessionId = null;
  let best = { found: false, w: 0, h: 0, nonFirstPixels: -1 };
  let launchErr = null;
  try {
    const s = await drvReq("POST", "/session", { capabilities: caps });
    sessionId = s.json && s.json.value && s.json.value.sessionId;
    if (!sessionId) throw new Error(s.json && s.json.value && s.json.value.message || "session not created");

    await drvReq("POST", `/session/${sessionId}/url`,
      { url: `http://127.0.0.1:${httpPort}/${name}.__paint__.html` });

    const deadline = Date.now() + TIMEOUT_MS;
    while (Date.now() < deadline) {
      const r = await drvReq("POST", `/session/${sessionId}/execute/sync`,
        { script: PROBE, args: [] });
      // A crashed browser or a closed session answers 4xx/5xx; stop polling now
      // instead of spinning to the full timeout with stale canvas state.
      if (r.status >= 400) {
        throw new Error((r.json && r.json.value && r.json.value.message) || `session lost (${r.status})`);
      }
      const p = r.json && r.json.value;
      if (p && p.found && (p.w > best.w || p.nonFirstPixels > best.nonFirstPixels)) best = p;
      if (p && p.found && p.w > 0 && p.h > 0 && p.nonFirstPixels > 16) break;
      await sleep(250);
    }
  } catch (e) {
    launchErr = e.message;
  } finally {
    if (sessionId) { try { await drvReq("DELETE", `/session/${sessionId}`); } catch {} }
  }

  const painted = best.w > 0 && best.h > 0 && best.nonFirstPixels > 16;
  const detail = `canvas=${best.w}x${best.h} nonFirstPixels=${best.nonFirstPixels}`;
  if (painted) {
    console.log(`  OK      ${name} (${detail})`);
  } else {
    console.error(`  FAIL    ${name} (${detail})`);
    if (best.err) console.error(`          printErr: ${String(best.err).slice(0, 200)}`);
    if (launchErr) console.error(`          driver: ${String(launchErr).slice(0, 200)}`);
    failed++;
  }
}

try { driver.kill(); } catch {}
server.close();

if (CALIBRATE) {
  console.log(`  (calibrate: ${failed} would-fail, not enforced)`);
  process.exit(0);
}
process.exit(failed ? 1 : 0);
