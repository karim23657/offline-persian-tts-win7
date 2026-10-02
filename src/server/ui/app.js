/*
 * win7-tts web interface.
 *
 * Kept in a separate file from index.html so the page can be cached and edited
 * without touching the markup, and so the embedding step has fewer escapes to
 * worry about.
 */
(function () {
  "use strict";

  var $ = function (id) { return document.getElementById(id); };
  var modelSel = $("model"), textEl = $("text"), statusEl = $("status"),
      barEl = $("bar"), player = $("player"), metaEl = $("meta"), healthEl = $("health");

  function setStatus(msg, kind) {
    statusEl.textContent = msg || "";
    statusEl.className = "status" + (kind ? " " + kind : "");
  }

  function fmt(n) { return (n < 10 ? n.toFixed(2) : Math.round(n)) + " s"; }

  function refreshHealth() {
    fetch("api/health")
      .then(function (r) { return r.json(); })
      .then(function (h) {
        healthEl.textContent = h.models_installed + " model(s) - " +
          h.models_loaded + " loaded - up " + h.uptime_s + "s";
      })
      .catch(function () { healthEl.textContent = "server not reachable"; });
  }

  function loadModels() {
    return fetch("api/models")
      .then(function (r) { return r.json(); })
      .then(function (d) {
        modelSel.innerHTML = "";
        (d.models || []).forEach(function (m) {
          var o = document.createElement("option");
          o.value = m.name;
          o.textContent = m.name + (m.loaded ? "  (loaded)" : "");
          modelSel.appendChild(o);
        });
        if (!(d.models || []).length) {
          var o = document.createElement("option");
          o.textContent = "no model installed";
          modelSel.appendChild(o);
          setStatus("Run scripts\\get_model.cmd gyro to download one.", "err");
        }
        return d;
      });
  }

  /* Read the sample rate out of the returned WAV so the meta line is real. */
  function wavRate(blob) {
    return blob.slice(0, 44).arrayBuffer().then(function (buf) {
      var v = new DataView(buf), rate = 22050, i = 12;
      while (i + 8 <= 44) {
        var id = String.fromCharCode(v.getUint8(i), v.getUint8(i + 1),
                                     v.getUint8(i + 2), v.getUint8(i + 3));
        var size = v.getUint32(i + 4, true);
        if (id === "fmt ") { rate = v.getUint32(i + 12, true); break; }
        i += 8 + size + (size & 1);
      }
      return rate;
    }).catch(function () { return 22050; });
  }

  function generate() {
    var text = textEl.value.trim();
    if (!text) { setStatus("Type something first.", "err"); return; }

    var t0 = performance.now();
    barEl.style.display = "block";
    barEl.value = 15;
    setStatus("generating...");
    $("go").disabled = true;

    fetch("api/synthesize", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        text: text,
        model: modelSel.value,
        speed: parseFloat($("speed").value),
        sid: parseInt($("sid").value, 10) || 0
      })
    })
      .then(function (r) {
        barEl.value = 70;
        if (!r.ok) {
          return r.json()
            .catch(function () { return {}; })
            .then(function (d) { throw new Error(d.error || ("HTTP " + r.status)); });
        }
        return r.blob();
      })
      .then(function (blob) {
        var elapsed = (performance.now() - t0) / 1000;
        player.src = URL.createObjectURL(blob);
        player.style.display = "block";
        barEl.value = 100;
        setStatus("ready in " + fmt(elapsed), "ok");
        return wavRate(blob).then(function (rate) {
          var secs = (blob.size - 44) / 2 / rate;
          metaEl.innerHTML =
            "<span>audio " + fmt(secs) + "</span>" +
            "<span>rate " + rate + " Hz</span>" +
            "<span>size " + Math.round(blob.size / 1024) + " KB</span>" +
            "<span>round trip " + fmt(elapsed) +
            (secs > 0 ? " (realtime x" + (secs / elapsed).toFixed(1) + ")" : "") +
            "</span>";
        });
      })
      .catch(function (e) {
        barEl.style.display = "none";
        setStatus(String(e.message || e), "err");
      })
      .then(function () {
        $("go").disabled = false;
        setTimeout(function () { barEl.style.display = "none"; }, 600);
        refreshHealth();
      });
  }

  function warm() {
    setStatus("loading model...");
    var t0 = performance.now();
    fetch("api/warmup", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: modelSel.value })
    })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (d.error) { setStatus(d.error, "err"); return; }
        setStatus("model " + d.model + " ready - loaded in " +
                  fmt((performance.now() - t0) / 1000), "ok");
        loadModels();
        refreshHealth();
      })
      .catch(function (e) { setStatus(String(e), "err"); });
  }

  $("speed").addEventListener("input", function () {
    $("speedVal").textContent = parseFloat(this.value).toFixed(2) + "x";
  });
  $("go").addEventListener("click", generate);
  $("warm").addEventListener("click", warm);
  $("sample").addEventListener("click", function () {
    textEl.value = "سلام دنیا! این یک آزمایش است و متن به صورت زنده تولید می‌شود.";
    textEl.focus();
  });

  loadModels().then(function () { setStatus("Ready."); });
  refreshHealth();
  setInterval(refreshHealth, 5000);
})();