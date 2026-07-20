/*
 * wstiles browser client: receive lossless RGBA tiles over a WebSocket and
 * blit them to a canvas; send mouse / keyboard input back in the binary wire
 * format the SDL wstiles driver expects (identical input protocol to jsmpeg).
 *
 * Configure the WebSocket endpoint by setting window.WSTILES_URL before load;
 * defaults to same host/port as the page.
 */
(function () {
  "use strict";

  // ---- wire constants (must match SDL_wstiles.c) ----
  var FRAME_TYPE_AV1 = 0x000001F1;
  var FRAME_TYPE_TITLE = 0x000001FC;
  var FRAME_TYPE_CURSOR = 0x000001FD;
  var FRAME_TYPE_CLIPBOARD = 0x000001FE;
  var FRAME_TYPE_TILE = 0x000001FF;

  var INPUT_KEY = 0x0001;
  var INPUT_MOUSE_BUTTON = 0x0002;
  var INPUT_MOUSE_ABSOLUTE = 0x0004;
  var INPUT_MOUSE_WHEEL = 0x0010;
  var INPUT_CONTROL = 0x0040;
  var INPUT_RESIZE = 0x0080;

  var KEY_DOWN = 0x0001;
  var KEY_PRESS = 0x0002;
  var KEY_RIGHT = 0x0004;
  var MOUSE_1_DOWN = 0x0002;
  var MOUSE_1_UP = 0x0004;
  var MOUSE_2_DOWN = 0x0008;
  var MOUSE_2_UP = 0x0010;

  var canvas = document.getElementById("screen");
  var ctx = canvas.getContext("2d");
  var status = document.getElementById("status");
  var haveSize = false;
  var ws = null;
  var videoDecoder = null;   // WebCodecs decoder when the driver streams AV1
  var av1ts = 0;

  function setupAV1(w, h) {
    if (typeof VideoDecoder === "undefined") {
      setStatus("AV1 mode but this browser lacks WebCodecs VideoDecoder");
      return;
    }
    // A resize re-configures the stream, so discard any decoder for the old size.
    if (videoDecoder) {
      try { videoDecoder.close(); } catch (e) {}
      videoDecoder = null;
    }
    videoDecoder = new VideoDecoder({
      output: function (frame) { try { ctx.drawImage(frame, 0, 0); } finally { frame.close(); } },
      error: function (e) { if (window.console) console.error("AV1 decode error:", e.message || e); }
    });
    // Profile 1 (High) = 4:4:4 8-bit; the keyframe's sequence header carries the rest.
    videoDecoder.configure({ codec: "av01.1.04M.08", codedWidth: w, codedHeight: h, optimizeForLatency: true });
  }

  function drawCompressed(ab, x, y, tw, th) {
    var stream = new Blob([ab]).stream().pipeThrough(new DecompressionStream("deflate"));
    new Response(stream).arrayBuffer().then(function (out) {
      ctx.putImageData(new ImageData(new Uint8ClampedArray(out), tw, th), x, y);
    }).catch(function (e) {
      if (window.console) console.error("wstiles tile inflate failed", e);
    });
  }

  function setStatus(s) { if (status) status.textContent = s; }

  // ---- live stats + control (drives the demo page's quality slider/meter) ----
  var statBytes = 0, statCodec = "tiles", statCQ = 30;
  window.wstilesSetCQ = function (cq) {
    statCQ = cq;
    var b = new ArrayBuffer(4), d = new DataView(b);
    d.setUint16(0, INPUT_CONTROL, true);
    d.setUint16(2, cq, true);
    if (ws && ws.readyState === 1) ws.send(b);
  };
  window.wstilesInfo = function () { return { codec: statCodec, cq: statCQ }; };

  // ---- auto-resize: keep the session the size of the space we can show it in ----
  // Opt out with window.WSTILES_AUTORESIZE = false before this script loads, for
  // apps that are laid out for one fixed size.
  var lastReqW = 0, lastReqH = 0, resizeTimer = null;
  var MARGIN = 8;   // keeps a scrollbar from appearing and re-triggering us

  function wantedSize() {
    // Measure against the viewport, not the canvas's parent: the parent is
    // often a shrink-to-fit wrapper whose width says nothing about the room
    // available. documentElement.clientWidth/Height exclude scrollbars.
    var docEl = document.documentElement;
    var vw = docEl.clientWidth || window.innerWidth || 0;
    var vh = docEl.clientHeight || window.innerHeight || 0;
    if (!vw || !vh) return [0, 0];
    var r = canvas.getBoundingClientRect();
    // Fill from wherever the canvas sits to the bottom-right of the viewport,
    // leaving a small margin so the page never gains a scrollbar (which would
    // shrink the viewport and trigger another resize).
    var w = Math.floor(vw - (r.left + (window.scrollX || 0)) - MARGIN);
    var h = Math.floor(vh - (r.top + (window.scrollY || 0)) - MARGIN);
    return [w, h];
  }

  function sendResize() {
    if (window.WSTILES_AUTORESIZE === false) return;
    if (!ws || ws.readyState !== 1) return;
    var s = wantedSize(), w = s[0], h = s[1];
    if (w < 160 || h < 160) return;
    // The driver rounds width down to a multiple of 16, so an exact match is
    // not expected; comparing against the last *request* is what stops a
    // request/resize/request feedback loop.
    if (w === lastReqW && h === lastReqH) return;
    // Hysteresis: ignore small deltas from the size we already have. Growing
    // the canvas can add a scrollbar, which shrinks the viewport, which would
    // ask for a smaller size, and so on -- a few pixels are not worth a loop.
    if (Math.abs(w - canvas.width) < 32 && Math.abs(h - canvas.height) < 32) return;
    lastReqW = w; lastReqH = h;
    var b = new ArrayBuffer(6), d = new DataView(b);
    d.setUint16(0, INPUT_RESIZE, true);
    d.setUint16(2, w, true);
    d.setUint16(4, h, true);
    ws.send(b);
  }

  window.addEventListener("resize", function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(sendResize, 250);   // settle before asking
  });
  setInterval(function () {
    if (typeof window.wstilesOnStats === "function") window.wstilesOnStats(statBytes, statCodec);
    statBytes = 0;
  }, 1000);

  // Each page load spawns its own private undroidwish (see WSTILES_SPAWN_URL);
  // that process self-terminates when this connection drops. WSTILES_URL is an
  // optional override for talking to a single persistent server instead.
  var spawnUrl = window.WSTILES_SPAWN_URL || null;
  var fixedUrl = window.WSTILES_URL ||
    (spawnUrl ? null
              : (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");

  function wsUrlForPort(port) {
    return (location.protocol === "https:" ? "wss://" : "ws://") +
      location.hostname + ":" + port + "/ws";
  }

  function startSession() {
    haveSize = false;
    lastReqW = lastReqH = 0;
    if (videoDecoder) { try { videoDecoder.close(); } catch (e) {} videoDecoder = null; }
    if (fixedUrl) { openWS(fixedUrl, 0); return; }
    if (!spawnUrl) { setStatus("no session endpoint configured"); return; }
    setStatus("starting a private undroidwish session…");
    fetch(spawnUrl, { credentials: "same-origin", cache: "no-store" })
      .then(function (r) { return r.json(); })
      .then(function (j) { openWS(wsUrlForPort(j.port), Date.now()); })
      .catch(function () { setStatus("couldn't start a session — retrying…"); setTimeout(startSession, 1500); });
  }

  function openWS(url, spawnedAt) {
    setStatus("connecting…");
    // The "ws" subprotocol binds to the driver's WebSocket handler (not HTTP).
    ws = new WebSocket(url, "ws");
    ws.binaryType = "arraybuffer";
    ws.onopen = function () { setStatus("connected"); };
    ws.onerror = function () {};
    ws.onmessage = onMessage;
    ws.onclose = function () {
      if (haveSize) {                        // had a live session -> it ended; start a fresh one
        setStatus("session ended — starting a new one…");
        setTimeout(startSession, 700);
      } else if (spawnedAt && Date.now() - spawnedAt < 8000) {
        setTimeout(function () { openWS(url, spawnedAt); }, 300);  // undroidwish still booting
      } else if (fixedUrl) {
        setStatus("disconnected — retrying…");
        setTimeout(function () { openWS(url, 0); }, 1000);
      } else {
        setStatus("session did not start — retrying…");
        setTimeout(startSession, 1500);
      }
    };
  }

  var connect = startSession;   // entry point (called at the bottom)

  function onMessage(ev) {
    var buf = ev.data;
    var dv = new DataView(buf);
    statBytes += buf.byteLength;

    // 'wtil' announces the framebuffer size. It arrives once on connect and
    // again after every resize, so it is accepted at any point in the stream.
    // Data frames start with a big-endian type of 0x000001Fx, which can never
    // collide with this magic.
    if (dv.getUint8(0) === 0x77 && dv.getUint8(1) === 0x74 &&
        dv.getUint8(2) === 0x69 && dv.getUint8(3) === 0x6c) {
      var w = dv.getUint16(4, false);
      var h = dv.getUint16(6, false);
      var codec = buf.byteLength >= 9 ? dv.getUint8(8) : 0;  // 0=tiles, 1=AV1
      canvas.width = w;      // also clears the canvas
      canvas.height = h;
      ctx.fillStyle = "#004e78";
      ctx.fillRect(0, 0, w, h);
      haveSize = true;
      statCodec = codec === 1 ? "av1" : "tiles";
      if (codec === 1) { setupAV1(w, h); setStatus("live (AV1) — " + w + "×" + h); }
      else setStatus("live (tiles) — " + w + "×" + h);
      // Ask for the size we can actually display, once layout has settled.
      setTimeout(sendResize, 0);
      return;
    }
    if (!haveSize) return;

    var type = dv.getUint32(0, false); // big endian
    // size = dv.getUint32(4,false); // == buf.byteLength

    if (type === FRAME_TYPE_AV1) {
      if (!videoDecoder) return;
      var key = dv.getUint8(8) === 1;
      var obu = buf.slice(9);
      try {
        videoDecoder.decode(new EncodedVideoChunk({
          type: key ? "key" : "delta", timestamp: av1ts++, data: obu
        }));
      } catch (e) { if (window.console) console.error("AV1 feed error:", e.message || e); }
    } else if (type === FRAME_TYPE_TILE) {
      var x = dv.getUint16(8, false);
      var y = dv.getUint16(10, false);
      var tw = dv.getUint16(12, false);
      var th = dv.getUint16(14, false);
      var fmt = dv.getUint8(16);            // 0 = raw RGBA, 1 = zlib deflate
      var data = buf.slice(17);
      // Tiles are a non-overlapping grid, so each can be drawn independently
      // as soon as its (possibly async) inflate completes.
      if (fmt === 0) {
        ctx.putImageData(new ImageData(new Uint8ClampedArray(data), tw, th), x, y);
      } else {
        drawCompressed(data, x, y, tw, th);
      }
    } else if (type === FRAME_TYPE_CURSOR) {
      var name = utf8(buf, 8);
      canvas.style.cursor = (name === "none") ? "none" : name;
    } else if (type === FRAME_TYPE_TITLE) {
      document.title = utf8(buf, 8);
    } else if (type === FRAME_TYPE_CLIPBOARD) {
      // server -> browser clipboard (best effort, ignored here)
    }
  }

  function utf8(buf, off) {
    var bytes = new Uint8Array(buf, off);
    try { return new TextDecoder("utf-8").decode(bytes); }
    catch (e) { return String.fromCharCode.apply(null, bytes); }
  }

  // ---- input ----
  function send(bytes) {
    if (ws && ws.readyState === 1) ws.send(bytes);
  }

  // Keyboard input needs the canvas focused, but focusing it must never move
  // the page: any scroll invalidates the coordinate mapping of the very event
  // being handled. preventScroll is honoured by current browsers; where it is
  // not, mapping before focusing (see mousedown) still keeps us correct.
  function focusCanvas() {
    try { canvas.focus({ preventScroll: true }); }
    catch (err) { canvas.focus(); }
  }

  function mouseXY(e) {
    var r = canvas.getBoundingClientRect();
    var sx = canvas.width / r.width;
    var sy = canvas.height / r.height;
    return [(e.clientX - r.left) * sx, (e.clientY - r.top) * sy];
  }

  function mouseMsg(type, flags, x, y) {
    var b = new ArrayBuffer(12);
    var d = new DataView(b);
    d.setUint16(0, type, true);
    d.setUint16(2, flags, true);
    d.setFloat32(4, x, true);
    d.setFloat32(8, y, true);
    return b;
  }

  canvas.addEventListener("mousemove", function (e) {
    var p = mouseXY(e);
    send(mouseMsg(INPUT_MOUSE_ABSOLUTE, 0, p[0], p[1]));
  });

  canvas.addEventListener("mousedown", function (e) {
    // Map BEFORE focusing. Focusing a tabindex'd canvas can scroll it into
    // view, and that scroll lands mid-dispatch: getBoundingClientRect() would
    // then report the moved rect while e.clientY still holds the pre-scroll
    // value, sending coordinates off by the scroll delta. That is enough to
    // miss a menubar entirely and click the widget below it.
    var p = mouseXY(e);
    focusCanvas();
    var flags = e.button === 2 ? MOUSE_2_DOWN : MOUSE_1_DOWN;
    send(mouseMsg(INPUT_MOUSE_BUTTON | INPUT_MOUSE_ABSOLUTE, flags, p[0], p[1]));
    e.preventDefault();
  });

  canvas.addEventListener("mouseup", function (e) {
    var p = mouseXY(e);
    var flags = e.button === 2 ? MOUSE_2_UP : MOUSE_1_UP;
    send(mouseMsg(INPUT_MOUSE_BUTTON | INPUT_MOUSE_ABSOLUTE, flags, p[0], p[1]));
    e.preventDefault();
  });

  canvas.addEventListener("contextmenu", function (e) { e.preventDefault(); });

  canvas.addEventListener("wheel", function (e) {
    var dx = e.deltaX ? (e.deltaX > 0 ? 1 : -1) : 0;
    var dy = e.deltaY ? (e.deltaY > 0 ? 1 : -1) : 0;
    send(mouseMsg(INPUT_MOUSE_WHEEL, 0, dx, dy));
    e.preventDefault();
  }, { passive: false });

  function keyMsg(flags, code) {
    var b = new ArrayBuffer(6);
    var d = new DataView(b);
    d.setUint16(0, INPUT_KEY, true);
    d.setUint16(2, flags, true);
    d.setUint16(4, code, true);
    return b;
  }

  function isRight(e) { return e.location === 2 /* KeyboardEvent.DOM_KEY_LOCATION_RIGHT */; }

  // e.keyCode is 0 for synthetic events (and legacy-deprecated); derive one
  // from e.key so Enter/Backspace/Tab/arrows work everywhere.
  var KEYMAP = {
    Enter: 13, Tab: 9, Backspace: 8, Escape: 27, " ": 32, Delete: 46,
    ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40,
    Home: 36, End: 35, PageUp: 33, PageDown: 34,
    Shift: 16, Control: 17, Alt: 18, Meta: 91, CapsLock: 20
  };
  function keyCodeOf(e) {
    if (e.keyCode) return e.keyCode;
    if (e.key in KEYMAP) return KEYMAP[e.key];
    if (e.key && e.key.length === 1) return e.key.toUpperCase().charCodeAt(0);
    return 0;
  }

  canvas.addEventListener("keydown", function (e) {
    var flags = KEY_DOWN | (isRight(e) ? KEY_RIGHT : 0);
    send(keyMsg(flags, keyCodeOf(e)));
    // printable character -> text
    if (e.key && e.key.length === 1) {
      var cp = e.key.charCodeAt(0);
      send(keyMsg(KEY_PRESS, cp));
    }
    // avoid the browser scrolling / navigating on control keys
    if (e.keyCode === 8 || e.keyCode === 9 || e.keyCode === 32 ||
        (e.keyCode >= 33 && e.keyCode <= 40)) {
      e.preventDefault();
    }
  });

  canvas.addEventListener("keyup", function (e) {
    var flags = isRight(e) ? KEY_RIGHT : 0;
    send(keyMsg(flags, keyCodeOf(e)));
  });

  canvas.setAttribute("tabindex", "0");
  canvas.addEventListener("click", function () { focusCanvas(); });

  connect();
})();
