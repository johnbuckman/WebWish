<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>undroidwish via web (wstiles)</title>
<style>
  html, body { margin: 0; background: #14181d; color: #dde3ea;
    font-family: -apple-system, Helvetica, Arial, sans-serif; }
  header { padding: 14px 20px 6px; }
  header h1 { margin: 0; font-size: 18px; font-weight: 600; }
  header p { margin: 4px 0 0; font-size: 13px; color: #8aa0b4; }
  #status { font-size: 12px; color: #7fd1a6; padding: 6px 20px 0; font-variant-numeric: tabular-nums; }
  #bar { display: flex; align-items: center; gap: 14px; padding: 8px 20px;
    font-size: 12px; color: #b7c4d2; flex-wrap: wrap; }
  #bar.hidden { display: none; }
  #bar label { display: flex; align-items: center; gap: 8px; }
  #cq { width: 240px; }
  #cqval, #kbps { font-variant-numeric: tabular-nums; color: #ffd27f; font-weight: 600; }
  .hint2 { color: #6b7d8c; }
  #stage { display: flex; justify-content: center; padding: 8px 16px 24px; }
  #screen { background: #004e78; box-shadow: 0 6px 30px rgba(0,0,0,.55);
    image-rendering: pixelated; max-width: 100%; outline: none;
    border-radius: 3px; }
  kbd { background:#222b33; border:1px solid #37424c; border-radius:3px;
    padding:1px 5px; font-size:11px; }
</style>
</head>
<body>
<header>
  <h1>undroidwish — rendered &amp; controlled through this web page</h1>
  <p>The Tcl/Tk app runs headless under the <code>wstiles</code> SDL driver; its
     framebuffer streams here (lossless tiles, or whole-screen AV1) over a
     WebSocket, and your mouse &amp; keyboard go back the same way. Click the
     canvas, then <kbd>type</kbd>, click the button, or drag on the sketch area.</p>
</header>
<div id="status">loading…</div>
<div id="bar" class="hidden">
  <label>AV1 quality
    <input type="range" id="cq" min="12" max="55" step="1" value="30">
  </label>
  <span>CQ <span id="cqval">30</span> <span class="hint2">(lower = sharper / bigger)</span></span>
  <span>· bandwidth <span id="kbps">0.0</span> KB/s</span>
</div>
<div id="stage">
  <canvas id="screen" width="800" height="600"></canvas>
</div>
<script>
  // Each page load spawns its own private undroidwish; the endpoint returns the
  // port it listens on. The process is killed when this connection drops.
  // Connect to stream.adp sitting NEXT TO this page, wherever that is — so the
  // whole directory can be dropped anywhere in a docroot with no edits. Also
  // upgrades to wss:// automatically when the page is served over https.
  (function () {
    var dir   = location.pathname.replace(/[^\/]*$/, "");   // ".../"  (works for /dir/ or /dir/index.adp)
    var proto = location.protocol === "https:" ? "wss://" : "ws://";
    window.WSTILES_URL = proto + location.host + dir + "stream.adp";
  })();

  // Live bandwidth meter (updated once/sec by wstiles.js).
  window.wstilesOnStats = function (bytes, codec) {
    document.getElementById("kbps").textContent = (bytes / 1024).toFixed(1);
    // Reveal the AV1 quality slider only when the stream is AV1.
    document.getElementById("bar").classList.toggle("hidden", codec !== "av1");
  };
</script>
<script src="wstiles.js"></script>
<script>
  // Wire the CQ slider to the live control channel.
  (function () {
    var cq = document.getElementById("cq"), cqval = document.getElementById("cqval");
    cq.addEventListener("input", function () {
      cqval.textContent = cq.value;
      if (window.wstilesSetCQ) window.wstilesSetCQ(parseInt(cq.value, 10));
    });
  })();
</script>
</body>
</html>
