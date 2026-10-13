/*
 * win7-tts web interface.
 *
 * Kept in a separate file from index.html so the page can be cached and edited
 * without touching the markup, and so the embedding step has fewer escapes to
 * worry about.
 *
 * Behaviours worth knowing about:
 *
 *  - Language: the page ships Persian (fa) and English (en), toggled by the
 *    header button and remembered in localStorage.  dir/lang follow the
 *    choice, and the textarea flips with it so typing stays RTL in Persian.
 *  - Model lifecycle: models are loaded once and reused for every generation
 *    (the server caches them).  Switching to another model tells the server
 *    to offload the previous one (POST /api/unload {"keep": ...}), and the
 *    page sends an unload beacon on pagehide, so closing the window returns
 *    the memory instead of leaving a 63 MB session resident.
 *  - ES5 on purpose: Windows 7 machines often run an old browser.
 */
(function () {
  "use strict";

  var $ = function (id) { return document.getElementById(id); };
  var modelSel = $("model"), textEl = $("text"), statusEl = $("status"),
      barEl = $("prog"), player = $("player"), metaEl = $("meta"),
      healthEl = $("health"), installedEl = $("installed"),
      catalogEl = $("catalog"), speedEl = $("speed"),
      genBtn = $("gen"), langBtn = $("langBtn"), playerWrap = $("playerWrap");

  /* ------------------------------------------------------------- i18n */
  var STR = {
    fa: {
      tagline: "گفتار فارسی، آفلاین و محلی",
      connecting: "در حال اتصال…",
      speak: "ساخت گفتار",
      textlabel: "متن فارسی",
      textph: "متن خود را اینجا بنویسید…",
      model: "مدل",
      speed: "سرعت گفتار",
      generate: "ساخت صدا ▶",
      generating: "در حال ساخت…",
      ready: "آماده پخش ✓",
      failed: "خطا: ",
      emptyText: "متن خالی است.",
      noModels: "مدلی نصب نیست — از پایین دانلود کنید یا scripts\\get_model.cmd را اجرا کنید.",
      auto: "پیش‌فرض (خودکار)",
      installed: "مدل‌های نصب‌شده",
      installedHint: "با تغییر مدل، مدل قبلی از حافظه آزاد می‌شود؛ هر مدل یک بار بارگذاری می‌شود و برای همهٔ ساخت‌ها می‌ماند.",
      catalog: "مدل‌های قابل دانلود",
      loading: "در حال بارگذاری…",
      catalogHint: "فایل‌ها را در پوشهٔ models بگذارید، یا برای نصب خودکار scripts\\get_model.cmd را اجرا کنید.",
      catalogNone: "فایل models.json پیدا نشد.",
      apidocs: "همهٔ مسیرها با یک اتصال HTTP/1.1 پایدار (keep-alive) سرو می‌شوند؛ مستندات کامل: /api/help",
      footer: "کاملاً محلی — بدون اینترنت، بدون حساب کاربری",
      health: "%1 مدل · %2 در حافظه · %3 ثانیه",
      unreachable: "سرور در دسترس نیست",
      loaded: "در حافظه",
      resident: "آزاد",
      meta: "%1 مگابایت WAV · %2 ثانیه ساخت",
      speeds: { "0.7": "0.7× (کند)", "0.85": "0.85×", "1": "1.0×", "1.15": "1.15×", "1.3": "1.3×", "1.5": "1.5× (سریع)" }
    },
    en: {
      tagline: "offline, on-device Persian speech",
      connecting: "connecting…",
      speak: "Text to speech",
      textlabel: "Persian text",
      textph: "Type your text here…",
      model: "Model",
      speed: "Speed",
      generate: "Generate ▶",
      generating: "Generating…",
      ready: "Ready ✓",
      failed: "Error: ",
      emptyText: "The text is empty.",
      noModels: "No model installed — download one below or run scripts\\get_model.cmd.",
      auto: "Default (auto)",
      installed: "Installed models",
      installedHint: "Switching models offloads the previous one; each model is loaded once and reused for every generation.",
      catalog: "Models available for download",
      loading: "Loading…",
      catalogHint: "Put the files into the models/ folder, or run scripts\\get_model.cmd for automatic install.",
      catalogNone: "models.json not found.",
      apidocs: "All routes are served over one HTTP/1.1 keep-alive connection; full reference: /api/help",
      footer: "fully local — no internet, no account",
      health: "%1 model(s) · %2 loaded · up %3s",
      unreachable: "server not reachable",
      loaded: "loaded",
      resident: "off",
      meta: "%1 MB WAV · generated in %2s",
      speeds: { "0.7": "0.7× (slow)", "0.85": "0.85×", "1": "1.0×", "1.15": "1.15×", "1.3": "1.3×", "1.5": "1.5× (fast)" }
    }
  };

  var lang = "fa";
  try {
    var saved = localStorage.getItem("win7tts-lang");
    if (saved === "fa" || saved === "en") lang = saved;
  } catch (e) { /* private mode: keep the default */ }
  var lastModels = [], lastCatalog = null, lastModel = null;

  function S() { return STR[lang]; }

  function applyI18n() {
    var t = S(), i, nodes;
    nodes = document.querySelectorAll("[data-i18n]");
    for (i = 0; i < nodes.length; i++) {
      if (t[nodes[i].getAttribute("data-i18n")] !== undefined) {
        nodes[i].textContent = t[nodes[i].getAttribute("data-i18n")];
      }
    }
    nodes = document.querySelectorAll("[data-i18n-ph]");
    for (i = 0; i < nodes.length; i++) {
      var k = nodes[i].getAttribute("data-i18n-ph");
      if (t[k] !== undefined) nodes[i].placeholder = t[k];
    }
    document.documentElement.lang = lang;
    document.documentElement.dir = (lang === "fa") ? "rtl" : "ltr";
    textEl.dir = (lang === "fa") ? "rtl" : "ltr";
    langBtn.textContent = (lang === "fa") ? "English" : "فارسی";
    for (i = 0; i < speedEl.options.length; i++) {
      var v = speedEl.options[i].value;
      speedEl.options[i].textContent = t.speeds[v] || (v + "×");
    }
    renderInstalled(lastModels);
    if (lastCatalog) renderCatalog(lastCatalog);
    refreshHealth();
  }

  langBtn.addEventListener("click", function () {
    lang = (lang === "fa") ? "en" : "fa";
    try { localStorage.setItem("win7tts-lang", lang); } catch (e) { }
    applyI18n();
  });

  /* ------------------------------------------------------------ status */
  function setStatus(msg, kind) {
    statusEl.textContent = msg || "";
    statusEl.className = "status" + (kind ? " " + kind : "");
  }

  function fmtBytes(n) {
    if (!n) return "—";
    if (n >= 1048576) return (n / 1048576).toFixed(1) + " MB";
    if (n >= 1024) return (n / 1024).toFixed(0) + " KB";
    return n + " B";
  }

  function setBusy(b) {
    genBtn.disabled = b;
  }

  /* ------------------------------------------------------------ health */
  function refreshHealth() {
    fetch("api/health")
      .then(function (r) { return r.json(); })
      .then(function (h) {
        healthEl.textContent = S().health
          .replace("%1", h.models_installed)
          .replace("%2", h.models_loaded)
          .replace("%3", h.uptime_s);
        healthEl.className = "pill ok";
      })
      .catch(function () {
        healthEl.textContent = S().unreachable;
        healthEl.className = "pill err";
      });
  }

  /* ---------------------------------------------------------- models */
  function renderInstalled(list) {
    var t = S(), i;
    lastModels = list || lastModels;
    installedEl.innerHTML = "";
    var modelOpt = document.createElement("option");
    modelOpt.value = "";
    modelOpt.textContent = t.auto;
    modelSel.innerHTML = "";
    modelSel.appendChild(modelOpt);

    if (!lastModels.length) {
      var li0 = document.createElement("li");
      li0.textContent = t.noModels;
      li0.style.cursor = "default";
      installedEl.appendChild(li0);
      return;
    }
    for (i = 0; i < lastModels.length; i++) {
      (function (m) {
        var li = document.createElement("li");
        li.title = m.name;
        var nm = document.createElement("span");
        nm.textContent = m.name;
        var bd = document.createElement("span");
        bd.className = "badge" + (m.loaded ? "" : " off");
        bd.textContent = m.loaded ? t.loaded : t.resident;
        li.appendChild(nm);
        li.appendChild(bd);
        li.addEventListener("click", function () {
          modelSel.value = m.name;
          var ev = document.createEvent("HTMLEvents");
          ev.initEvent("change", true, false);
          modelSel.dispatchEvent(ev);
        });
        installedEl.appendChild(li);

        var opt = document.createElement("option");
        opt.value = m.name;
        opt.textContent = m.name + (m.loaded ? " · " + t.loaded : "");
        modelSel.appendChild(opt);
      })(lastModels[i]);
    }
    if (lastModel) modelSel.value = lastModel;
    modelOpt = null;
  }

  function loadModels() {
    fetch("api/models")
      .then(function (r) { return r.json(); })
      .then(function (d) { renderInstalled(d.models || []); })
      .catch(function () { });
  }

  /* Offload the *previous* model when the selection changes: the chosen one
     is loaded once and stays resident for many generations, the old one is
     freed immediately instead of lingering until the LRU notices. */
  function unloadExcept(keep) {
    fetch("api/unload", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ keep: keep || "" })
    }).catch(function () { });
  }

  modelSel.addEventListener("change", function () {
    var newModel = modelSel.value;
    if (lastModel !== null && newModel && newModel !== lastModel) {
      unloadExcept(newModel);
      loadModels();
    }
    lastModel = newModel;
  });

  /* --------------------------------------------------------- catalog */
  function renderCatalog(items) {
    var t = S(), i, groups = {}, order = [];
    lastCatalog = items;
    catalogEl.innerHTML = "";

    var hint = document.createElement("div");
    hint.className = "cat-desc";
    hint.textContent = t.catalogHint;
    catalogEl.appendChild(hint);

    if (!items || !items.length) {
      var none = document.createElement("div");
      none.className = "status";
      none.textContent = t.catalogNone;
      catalogEl.appendChild(none);
      return;
    }

    /* One group per installable model: a plain file's folder, or an archive's
       extract target.  Files of one model travel together. */
    for (i = 0; i < items.length; i++) {
      var it = items[i];
      var key = (it.kind === "archive")
        ? it.name.replace(/\.tar\.bz2$/, "")
        : (it.dir.split("/").pop() || it.dir);
      if (!groups[key]) { groups[key] = { desc: it.desc || "", files: [] }; order.push(key); }
      groups[key].files.push(it);
      if (!groups[key].desc && it.desc) groups[key].desc = it.desc;
    }

    var installedNames = {};
    for (i = 0; i < lastModels.length; i++) installedNames[lastModels[i].name] = 1;

    for (i = 0; i < order.length; i++) {
      var key2 = order[i], g = groups[key2];
      var box = document.createElement("div");
      box.className = "cat-group";
      var title = document.createElement("div");
      title.className = "cat-title";
      title.textContent = key2;
      box.appendChild(title);
      if (g.desc) {
        var desc = document.createElement("div");
        desc.className = "cat-desc";
        desc.textContent = g.desc;
        box.appendChild(desc);
      }
      var j;
      for (j = 0; j < g.files.length; j++) {
        (function (f) {
          var row = document.createElement("div");
          row.className = "cat-file";
          var a = document.createElement("a");
          a.href = f.url;
          a.textContent = f.name;
          a.setAttribute("target", "_blank");
          a.setAttribute("rel", "noopener");
          var sz = document.createElement("span");
          sz.className = "cat-size";
          sz.textContent = fmtBytes(f.size);
          row.appendChild(a);
          row.appendChild(sz);
          if (installedNames[key2]) {
            var ok = document.createElement("span");
            ok.className = "cat-installed";
            ok.textContent = "✓ " + t.loaded;
            row.appendChild(ok);
          }
          box.appendChild(row);
        })(g.files[j]);
      }
      catalogEl.appendChild(box);
    }
  }

  function loadCatalog() {
    fetch("api/catalog")
      .then(function (r) {
        if (!r.ok) throw new Error("no catalog");
        return r.json();
      })
      .then(function (d) { renderCatalog(d.items || []); })
      .catch(function () { renderCatalog(null); });
  }

  /* -------------------------------------------------------- generate */
  function generate() {
    var text = (textEl.value || "").replace(/^\s+|\s+$/g, "");
    if (!text) { setStatus(S().emptyText, "err"); return; }

    var t0 = Date.now();
    var speed = parseFloat(speedEl.value) || 1;
    setBusy(true);
    barEl.className = "prog on";
    setStatus(S().generating, "");

    fetch("api/synthesize", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ text: text, model: modelSel.value || undefined, speed: speed })
    }).then(function (r) {
      if (!r.ok) {
        return r.json().then(function (e) {
          throw new Error(e.error || ("HTTP " + r.status));
        }, function () { throw new Error("HTTP " + r.status); });
      }
      return r.blob();
    }).then(function (blob) {
      var url = URL.createObjectURL(blob);
      player.src = url;
      var p = player.play();
      if (p && p.catch) p.catch(function () { /* autoplay blocked: user presses play */ });
      var secs = (Date.now() - t0) / 1000;
      metaEl.textContent = S().meta
        .replace("%1", (blob.size / 1048576).toFixed(1))
        .replace("%2", secs.toFixed(2));
      playerWrap.className = "player-wrap";
      barEl.className = "prog";
      setStatus(S().ready, "ok");
      setBusy(false);
    }).catch(function (err) {
      setStatus(S().failed + (err && err.message ? err.message : err), "err");
      barEl.className = "prog";
      setBusy(false);
    });
  }

  genBtn.addEventListener("click", generate);
  textEl.addEventListener("keydown", function (ev) {
    if (ev.key === "Enter" && (ev.ctrlKey || ev.metaKey)) generate();
  });

  /* Closing the tab/page hands the memory back instead of leaving the model
     resident until the server exits (the server answers and frees engines). */
  window.addEventListener("pagehide", function () {
    try { navigator.sendBeacon("api/unload", "{}"); } catch (e) { }
  });

  /* ----------------------------------------------------------- start */
  applyI18n();
  loadModels();
  loadCatalog();
  refreshHealth();
  setInterval(refreshHealth, 10000);
  setInterval(loadModels, 15000);
})();
