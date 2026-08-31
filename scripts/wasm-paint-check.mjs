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
//        only, never fail), WASM_PAINT_STRICT_INPUT=0 (downgrade xwpe input
//        failure to a warning), WASM_PAINT_WEBDRIVER (driver binary path),
//        WASM_PAINT_REQUEST_TIMEOUT_MS (default 15000, per driver request),
//        WASM_PAINT_ATTEMPTS (default 2, retries of a transient failure).

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
// A malformed budget must not silently disable a check or spin forever, so
// anything that is not a finite positive integer falls back to the documented
// default rather than propagating NaN or Infinity into a loop bound.
function positiveInt(value, fallback) {
  const parsed = Number(value);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : fallback;
}
const TIMEOUT_MS = positiveInt(process.env.WASM_PAINT_TIMEOUT_MS, 20000);
// A probe or action that outlives this is treated as a stuck driver. The
// budget is generous because a loaded CI runner can stall a WebKit request for
// several seconds without anything being wrong.
const REQUEST_TIMEOUT_MS =
  positiveInt(process.env.WASM_PAINT_REQUEST_TIMEOUT_MS, 15000);
// Retries cover only what a loaded runner can do before anything renders. Once
// a frame exists, every later outcome is a real result: see the verdict below.
const ATTEMPTS = positiveInt(process.env.WASM_PAINT_ATTEMPTS, 2);
const CALIBRATE = process.env.WASM_PAINT_CALIBRATE === "1";
const KEEP_BROWSER = process.env.WASM_PAINT_KEEP_BROWSER === "1";
const STRICT_INPUT = process.env.WASM_PAINT_STRICT_INPUT !== "0";
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
  thisProgram: ${JSON.stringify(name === "xwpe" || name === "xcircuit" ? name : undefined)},
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

const CANVAS_HASH = `
  var c = document.getElementById("canvas");
  if (!c || c.width <= 0 || c.height <= 0) return null;
  var sw = Math.min(c.width, 128), sh = Math.min(c.height, 96);
  var off = document.createElement("canvas");
  off.width = sw; off.height = sh;
  var ctx = off.getContext("2d");
  ctx.drawImage(c, 0, 0, c.width, c.height, 0, 0, sw, sh);
  var d = ctx.getImageData(0, 0, sw, sh).data;
  var h = 2166136261;
  for (var i = 0; i < d.length; i++) h = Math.imul(h ^ d[i], 16777619);
  return h >>> 0;
`;

const XWPE_MOUSE_POINT = `
  var c = document.getElementById("canvas");
  var r = c.getBoundingClientRect();
  var sx = r.width / c.width, sy = r.height / c.height;
  var x = Math.round(80 * sx - r.width / 2);
  var y = Math.round(16 * sy - r.height / 2);
  return { x: x, y: y };
`;

const MOSAIC_WHEEL_AND_MENU = `
  var c = document.getElementById("canvas");
  var r = c.getBoundingClientRect();
  function h() {
    if (!c || !c.width || !c.height) return null;
    var off = document.createElement("canvas");
    off.width = 160; off.height = 120;
    var x = off.getContext("2d");
    x.drawImage(c, 0, 0, 160, 120);
    var d = x.getImageData(0, 0, 160, 120).data;
    var out = 2166136261;
    for (var i = 0; i < d.length; i++) out = Math.imul(out ^ d[i], 16777619);
    return out >>> 0;
  }
  var before = h();
  c.dispatchEvent(new WheelEvent("wheel", {
    bubbles: true, cancelable: true,
    clientX: r.left + r.width / 2,
    clientY: r.top + r.height / 2,
    deltaY: 480
  }));
  var fileX = r.left + 30, fileY = r.top + 18;
  ["mousedown", "mouseup"].forEach(function(type) {
    c.dispatchEvent(new MouseEvent(type, {
      bubbles: true, cancelable: true, button: 0,
      clientX: fileX, clientY: fileY
    }));
  });
  return { before: before };
`;

const MOSAIC_HASH = `
  var c = document.getElementById("canvas");
  if (!c || !c.width || !c.height) return null;
  var off = document.createElement("canvas");
  off.width = 160; off.height = 120;
  var x = off.getContext("2d");
  x.drawImage(c, 0, 0, 160, 120);
  var d = x.getImageData(0, 0, 160, 120).data;
  var out = 2166136261;
  for (var i = 0; i < d.length; i++) out = Math.imul(out ^ d[i], 16777619);
  return out >>> 0;
`;

const XCIRCUIT_MENU_BAR = `
  var c = document.getElementById("canvas");
  if (!c || !c.width || !c.height) return null;
  var off = document.createElement("canvas");
  off.width = 360; off.height = 56;
  var ctx = off.getContext("2d");
  ctx.drawImage(c, 0, 0, 720, 112, 0, 0, 360, 56);
  var d = ctx.getImageData(0, 0, 360, 56).data;
  var dark = 0;
  for (var i = 0; i < d.length; i += 4)
    if (d[i] < 24 && d[i + 1] < 24 && d[i + 2] < 24) dark++;
  return { dark: dark, total: d.length / 4 };
`;

const XCIRCUIT_MENU_OPEN = `
  var callback = arguments[arguments.length - 1];
  var c = document.getElementById("canvas");
  if (!c || !c.width || !c.height) return callback(null);
  window.scrollTo(0, 0);
  var r = c.getBoundingClientRect();
  function h() {
    var off = document.createElement("canvas");
    off.width = 420; off.height = 360;
    var ctx = off.getContext("2d");
    ctx.drawImage(c, 0, 0, 420, 360, 0, 0, 420, 360);
    var d = ctx.getImageData(0, 0, 420, 360).data;
    var out = 2166136261;
    for (var i = 0; i < d.length; i++)
      out = Math.imul(out ^ d[i], 16777619);
    return out >>> 0;
  }
  function popupLeft() {
    var off = document.createElement("canvas");
    off.width = 420; off.height = 300;
    var ctx = off.getContext("2d");
    ctx.drawImage(c, 0, 0, 420, 300, 0, 0, 420, 300);
    var d = ctx.getImageData(0, 0, 420, 300).data;
    for (var x = 32; x < 180; x++) {
      var hits = 0;
      for (var y = 60; y < 260; y++) {
        var i = (y * 420 + x) * 4;
        if (d[i] > 220 && d[i] < 252 &&
            d[i + 1] > 220 && d[i + 1] < 252 &&
            d[i + 2] > 185 && d[i + 2] < 242)
          hits++;
      }
      if (hits > 120)
        return x;
    }
    return -1;
  }
  var before = h();
  c.dispatchEvent(new MouseEvent("mousemove", {
    bubbles: true, cancelable: true, button: 0,
    clientX: r.left + 60, clientY: r.top + 18
  }));
  c.dispatchEvent(new MouseEvent("mousedown", {
    bubbles: true, cancelable: true, button: 0,
    clientX: r.left + 60, clientY: r.top + 18
  }));
  setTimeout(function() {
    callback({
      before: before, after: h(), popupLeft: popupLeft(),
      rect: { left: r.left, top: r.top }
    });
  }, 1000);
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
function drvReq(method, path, body, timeoutMs) {
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
    // Navigation blocks until the page load event, so it gets the paint budget;
    // ordinary probe/action calls keep the short default.
    req.setTimeout(timeoutMs || REQUEST_TIMEOUT_MS,
      () => req.destroy(new Error("driver request timeout")));
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
process.on("exit", () => { if (!KEEP_BROWSER) try { driver.kill(); } catch {} });
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

  let attempt = 0;
  let transient = "";
  for (;;) {
  attempt++;
  let sessionId = null;
  let best = { found: false, w: 0, h: 0, nonFirstPixels: -1 };
  let launchErr = null;
  let inputChanged = name !== "xwpe" && name !== "mosaic" && name !== "xcircuit";
  let inputErr = null;
  let inputDetail = "";
  let mouseQueueLen = 0;
  let layoutErr = null;
  let visualErr = null;
  try {
    const s = await drvReq("POST", "/session", { capabilities: caps });
    sessionId = s.json && s.json.value && s.json.value.sessionId;
    if (!sessionId) throw new Error(s.json && s.json.value && s.json.value.message || "session not created");

    const page = (name === "xwpe" || name === "mosaic" || name === "xcircuit")
      ? `${name}.html`
      : `${name}.__paint__.html`;
    await drvReq("POST", `/session/${sessionId}/url`,
      { url: `http://127.0.0.1:${httpPort}/${page}` }, TIMEOUT_MS);

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
      const readyPixels = name === "xcircuit" ? 1000 : 16;
      if (p && p.found && p.w > 0 && p.h > 0 &&
          p.nonFirstPixels > readyPixels) {
        break;
      }
      await sleep(250);
    }
    if (name === "xwpe" && best.w > 0 && best.h > 0 && best.nonFirstPixels > 16) {
      await sleep(1000);
      const before = await drvReq("POST", `/session/${sessionId}/execute/sync`,
        { script: CANVAS_HASH, args: [] });
      const beforeHash = before.json && before.json.value;
      const pointResponse = await drvReq("POST", `/session/${sessionId}/execute/sync`,
        { script: XWPE_MOUSE_POINT, args: [] });
      const point = pointResponse.json && pointResponse.json.value;
      const canvasResponse = await drvReq("POST", `/session/${sessionId}/element`,
        { using: "css selector", value: "#canvas" });
      const canvasElement = canvasResponse.json && canvasResponse.json.value;
      await drvReq("POST", `/session/${sessionId}/actions`, { actions: [{
        type: "pointer", id: "xwpe-mouse",
        parameters: { pointerType: "mouse" },
        actions: [
          { type: "pointerMove", duration: 0, origin: canvasElement, x: point.x, y: point.y },
          { type: "pointerDown", button: 0 },
          { type: "pause", duration: 500 },
          { type: "pointerUp", button: 0 }
        ]
      }] });
      const clickDeadline = Date.now() + 2500;
      while (Date.now() < clickDeadline) {
        await sleep(250);
        const after = await drvReq("POST", `/session/${sessionId}/execute/sync`,
          { script: CANVAS_HASH, args: [] });
        const afterHash = after.json && after.json.value;
        if (beforeHash !== null && afterHash !== null && afterHash !== beforeHash) {
          inputChanged = true;
          inputDetail = " input=mouse-menu-click";
          break;
        }
      }
      if (!inputChanged) {
        await drvReq("POST", `/session/${sessionId}/execute/sync`, { script: `
          var c = document.getElementById("canvas");
          c.focus();
          ["keydown", "keyup"].forEach(function(type) {
            c.dispatchEvent(new KeyboardEvent(type, {
              bubbles: true, cancelable: true,
              key: "ArrowDown", code: "ArrowDown"
            }));
          });
        `, args: [] });
        await sleep(250);
        const after = await drvReq("POST", `/session/${sessionId}/execute/sync`,
          { script: CANVAS_HASH, args: [] });
        const afterHash = after.json && after.json.value;
        if (beforeHash !== null && afterHash !== null && afterHash !== beforeHash) {
          inputChanged = true;
          inputDetail = " input=key-arrow";
        }
      }
      const queuedResponse = await drvReq("POST", `/session/${sessionId}/execute/sync`,
        { script: "return (window.__libx11CompatDomQueue || []).length;", args: [] });
      mouseQueueLen = queuedResponse.json && queuedResponse.json.value || 0;
      if (!inputChanged)
        inputErr = `mouse click did not change canvas (queued ${mouseQueueLen})`;
      if (inputErr && !STRICT_INPUT) {
        inputChanged = true;
        inputDetail = " input=mouse-unverified";
      }
    }
    if (name === "mosaic" && best.w > 0 && best.h > 0 && best.nonFirstPixels > 16) {
      const layout = await drvReq("POST", `/session/${sessionId}/execute/sync`,
        { script: "var c=document.getElementById('canvas'),r=c.getBoundingClientRect();return {right:r.right,bottom:r.bottom,iw:innerWidth,ih:innerHeight};", args: [] });
      const value = layout.json && layout.json.value;
      if (value && (value.right > value.iw || value.bottom > value.ih))
        layoutErr = `canvas clipped (${Math.ceil(value.right)}x${Math.ceil(value.bottom)} > ${value.iw}x${value.ih})`;
      const before = await drvReq("POST", `/session/${sessionId}/execute/sync`,
        { script: MOSAIC_WHEEL_AND_MENU, args: [] });
      const inputBefore = before.json && before.json.value;
      const baseline = inputBefore && typeof inputBefore.before === "number"
        ? inputBefore.before
        : null;
      // Poll for the canvas to change and re-dispatch the wheel/menu input each
      // round rather than checking once after a fixed wait: under a real browser
      // a single scroll or menu press is timing-flaky, so keep nudging until the
      // repaint lands or the deadline passes.
      const inputDeadline = Date.now() + 8000;
      while (baseline !== null && Date.now() < inputDeadline) {
        await sleep(500);
        const afterHash = await drvReq("POST", `/session/${sessionId}/execute/sync`,
          { script: MOSAIC_HASH, args: [] });
        const h = afterHash.status === 200 && afterHash.json
          ? afterHash.json.value
          : null;
        if (typeof h === "number" && h !== baseline) {
          inputChanged = true;
          inputDetail = " input=wheel-menu";
          break;
        }
        await drvReq("POST", `/session/${sessionId}/execute/sync`,
          { script: MOSAIC_WHEEL_AND_MENU, args: [] });
      }
      if (!inputChanged)
        inputErr = "wheel/menu input did not change Mosaic canvas";
    }
    if (name === "xcircuit" && best.w > 0 && best.h > 0 && best.nonFirstPixels > 16) {
      const menu = await drvReq("POST", `/session/${sessionId}/execute/sync`,
        { script: XCIRCUIT_MENU_BAR, args: [] });
      const value = menu.json && menu.json.value;
      if (!value || value.dark > value.total / 4)
        visualErr = `menu bar dark pixels ${value ? value.dark : "?"}/${value ? value.total : "?"}`;
      const before = await drvReq("POST", `/session/${sessionId}/execute/async`,
        { script: XCIRCUIT_MENU_OPEN, args: [] });
      const inputBefore = before.json && before.json.value;
      if (!inputBefore || inputBefore.rect.left < 0 || inputBefore.rect.top < 0)
        layoutErr = `canvas clipped at ${inputBefore ? inputBefore.rect.left : "?"},${inputBefore ? inputBefore.rect.top : "?"}`;
      else if (inputBefore.after === inputBefore.before)
        inputErr = "File menu mousedown did not change XCircuit canvas";
      else if (inputBefore.popupLeft < 0 || inputBefore.popupLeft > 45)
        inputErr = `File menu opened at x=${inputBefore.popupLeft}`;
      else {
        inputChanged = true;
        inputDetail = " input=file-menu";
      }
    }
  } catch (e) {
    launchErr = e.message;
  } finally {
    if (sessionId && !KEEP_BROWSER) { try { await drvReq("DELETE", `/session/${sessionId}`); } catch {} }
  }

  const painted = best.w > 0 && best.h > 0 && best.nonFirstPixels > 16;
  const detail = `canvas=${best.w}x${best.h} nonFirstPixels=${best.nonFirstPixels}`;
  const inputOk = inputChanged && (!inputErr || !STRICT_INPUT);
  if (painted && inputOk && !layoutErr && !visualErr && !launchErr) {
    console.log(`  OK      ${name} (${detail}${inputDetail}${
      attempt > 1 ? ` attempt=${attempt}` : ""})`);
    if (inputErr && !STRICT_INPUT)
      console.error(`          WARN input: ${inputErr}`);
    break;
  }

  /* Retry only a run that never produced a frame: that is what a loaded runner
   * does to a healthy build. Once the canvas has painted, every later outcome
   * is a real result and is reported on this attempt, including a driver
   * request that hung during an input action. Keying this on the error alone
   * would let a post-paint hang be retried away by a luckier attempt.
   */
  transient = painted ? ""
              : launchErr ? `driver: ${String(launchErr).slice(0, 120)}`
                          : `no frame (${detail})`;
  if (transient && attempt < ATTEMPTS) {
    console.error(`  RETRY   ${name} (${transient}; attempt ${attempt}/${ATTEMPTS})`);
    await sleep(1000);
    continue;
  }

  console.error(`  FAIL    ${name} (${detail}${
    attempt > 1 ? ` after ${attempt} attempts` : ""})`);
  if (best.err) console.error(`          printErr: ${String(best.err).slice(0, 200)}`);
  if (inputErr) console.error(`          input: ${inputErr}`);
  if (layoutErr) console.error(`          layout: ${layoutErr}`);
  if (visualErr) console.error(`          visual: ${visualErr}`);
  if (launchErr) console.error(`          driver: ${String(launchErr).slice(0, 200)}`);
  failed++;
  break;
  }
}

if (!KEEP_BROWSER) try { driver.kill(); } catch {}
server.close();

if (CALIBRATE) {
  console.log(`  (calibrate: ${failed} would-fail, not enforced)`);
  process.exit(0);
}
process.exit(failed ? 1 : 0);
