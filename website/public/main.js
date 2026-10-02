// Keeps the download links, sizes and hashes in sync with the latest GitHub release.
// The page ships with the values of the current release, so it still works if this fails.
(function () {
  "use strict";

  var API = "https://api.github.com/repos/ExecSam/Mufus/releases/latest";
  var CACHE_KEY = "mufus-latest-release";
  var CACHE_TTL = 30 * 60 * 1000;
  var ASSETS = {
    x64: /^mufus-[\d.]+\.exe$/i,
    x86: /^mufus-[\d.]+_x86\.exe$/i
  };

  function $$(selector) {
    return Array.prototype.slice.call(document.querySelectorAll(selector));
  }

  function formatSize(bytes) {
    return (bytes / 1048576).toFixed(1) + " MB";
  }

  function shortHash(hash) {
    return hash.slice(0, 8) + "…" + hash.slice(-8);
  }

  function apply(release) {
    var version = release.tag_name.replace(/^v/i, "");
    $$("[data-version]").forEach(function (el) { el.textContent = version; });
    $$("[data-notes]").forEach(function (el) { el.href = release.html_url; });

    var date = new Date(release.published_at);
    if (!isNaN(date)) {
      $$("[data-date]").forEach(function (el) {
        el.dateTime = release.published_at.slice(0, 10);
        el.textContent = date.toLocaleDateString("en-GB", { day: "numeric", month: "long", year: "numeric" });
      });
    }

    Object.keys(ASSETS).forEach(function (arch) {
      var asset = (release.assets || []).filter(function (a) { return ASSETS[arch].test(a.name); })[0];
      if (!asset) return;
      $$('[data-dl="' + arch + '"]').forEach(function (el) { el.href = asset.browser_download_url; });
      $$('[data-name="' + arch + '"]').forEach(function (el) { el.textContent = asset.name; });
      $$('[data-size="' + arch + '"]').forEach(function (el) { el.textContent = formatSize(asset.size); });
      var hash = /^sha256:([0-9a-f]{64})$/i.exec(asset.digest || "");
      if (hash) {
        $$('[data-sha="' + arch + '"]').forEach(function (el) {
          el.textContent = shortHash(hash[1]);
          el.title = hash[1];
        });
        $$('[data-copy-sha="' + arch + '"]').forEach(function (el) { el.setAttribute("data-copy", hash[1]); });
      }
    });
  }

  function readCache() {
    try {
      var cached = JSON.parse(localStorage.getItem(CACHE_KEY));
      if (cached && Date.now() - cached.time < CACHE_TTL) return cached.release;
    } catch (e) { /* storage unavailable */ }
    return null;
  }

  function writeCache(release) {
    try {
      localStorage.setItem(CACHE_KEY, JSON.stringify({ time: Date.now(), release: release }));
    } catch (e) { /* storage unavailable */ }
  }

  function loadRelease() {
    var cached = readCache();
    if (cached) return apply(cached);
    if (!window.fetch) return;
    fetch(API, { headers: { Accept: "application/vnd.github+json" } })
      .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
      .then(function (data) {
        // Only keep what the page uses
        var release = {
          tag_name: data.tag_name,
          html_url: data.html_url,
          published_at: data.published_at,
          assets: (data.assets || []).map(function (a) {
            return { name: a.name, size: a.size, digest: a.digest, browser_download_url: a.browser_download_url };
          })
        };
        if (!release.tag_name) return;
        writeCache(release);
        apply(release);
      })
      .catch(function () { /* keep the built-in values */ });
  }

  function initCopy() {
    if (!navigator.clipboard) return;
    $$("[data-copy]").forEach(function (button) {
      button.hidden = false;
      button.addEventListener("click", function () {
        navigator.clipboard.writeText(button.getAttribute("data-copy")).then(function () {
          button.textContent = "Copied";
          button.classList.add("copied");
          setTimeout(function () {
            button.textContent = "Copy";
            button.classList.remove("copied");
          }, 1500);
        });
      });
    });
  }

  initCopy();
  loadRelease();
})();
