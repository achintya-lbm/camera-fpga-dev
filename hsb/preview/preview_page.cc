#include "hsb/preview/preview_page.hpp"

namespace hsb::preview {

namespace {
constexpr const char kPage[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>__TITLE__</title>
<style>
  body { margin: 0; font: 14px/1.4 system-ui, sans-serif; background: #111; color: #ddd; display: flex; height: 100vh; }
  #view { flex: 1; display: flex; align-items: center; justify-content: center; background: #000; overflow: hidden; }
  #view img { max-width: 100%; max-height: 100%; }
  #panel { width: 340px; padding: 14px 16px; overflow-y: auto; background: #1b1b1b; border-left: 1px solid #333; }
  h1 { font-size: 16px; margin: 0 0 12px; }
  h2 { font-size: 12px; text-transform: uppercase; letter-spacing: .06em; color: #999; margin: 18px 0 6px; }
  .ctl { margin: 8px 0; }
  .ctl label { display: flex; justify-content: space-between; margin-bottom: 2px; }
  .ctl input[type=range] { width: 100%; }
  .ctl input[type=number] { width: 84px; background: #222; color: #eee; border: 1px solid #444; padding: 2px 4px; }
  button { background: #2d6cdf; color: #fff; border: 0; padding: 7px 12px; margin: 4px 4px 4px 0; cursor: pointer; border-radius: 3px; }
  button.secondary { background: #444; }
  #status { font-family: ui-monospace, monospace; font-size: 12px; white-space: pre; color: #9c9; }
  #msg { color: #f99; min-height: 1.2em; }
  #files a { color: #8bf; text-decoration: none; display: block; font-family: ui-monospace, monospace; font-size: 12px; }
</style>
</head>
<body>
<div id="view"><img id="stream" src="/stream.mjpg" alt="live preview"></div>
<div id="panel">
  <h1>__TITLE__</h1>
  <div id="ident"></div>
  <h2>Sensor</h2>
  <div class="ctl"><label>Exposure (ms) <input type="number" id="exposure_ms_n" step="0.1" min="0"></label>
    <input type="range" id="exposure_ms" min="0" max="33" step="0.05"></div>
  <div class="ctl"><label>Gain (dB) <input type="number" id="gain_db_n" step="0.3" min="0" max="72"></label>
    <input type="range" id="gain_db" min="0" max="72" step="0.3"></div>
  <div class="ctl"><label>Black level (10-bit units) <input type="number" id="black_level_n" step="1" min="0" max="1023"></label>
    <input type="range" id="black_level" min="0" max="1023" step="1"></div>
  <div class="ctl"><label><span>Test pattern</span><input type="checkbox" id="test_pattern"></label>
    <label>Pattern <select id="test_pattern_select">
      <option value="0">0</option><option value="1">1</option><option value="2">2</option><option value="3">3</option>
      <option value="4">4</option><option value="5">5</option><option value="6">6</option><option value="7">7</option>
    </select></label></div>
  <div id="msg"></div>
  <h2>Capture</h2>
  <button id="still">Still (full-res JPEG)</button>
  <button id="capture" class="secondary">Capture raw</button>
  <button id="snap" class="secondary">Open snapshot</button>
  <div id="capmsg"></div>
  <h2>Status</h2>
  <div id="status">…</div>
  <h2>Files</h2>
  <div id="files"></div>
</div>
<script>
(function () {
  const keys = ['exposure_ms', 'gain_db', 'black_level'];
  const editing = {};
  let timer = null;
  const $ = (id) => document.getElementById(id);

  function post(url, body) {
    return fetch(url, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)})
      .then(r => r.json().then(j => ({ok: r.ok, ...j})));
  }
  function sendControl(kv) {
    clearTimeout(timer);
    timer = setTimeout(() => {
      post('/control', kv).then(j => { $('msg').textContent = j.ok ? '' : ('error: ' + (j.error || 'unknown')); })
        .catch(e => { $('msg').textContent = 'error: ' + e; });
    }, 150);
  }
  keys.forEach(k => {
    const slider = $(k), num = $(k + '_n');
    const onInput = (src) => { const v = src.value; slider.value = v; num.value = v; editing[k] = Date.now(); sendControl({[k]: v}); };
    slider.addEventListener('input', () => onInput(slider));
    num.addEventListener('change', () => onInput(num));
  });
  $('test_pattern').addEventListener('change', e => sendControl({test_pattern: e.target.checked ? 1 : 0}));
  $('test_pattern_select').addEventListener('change', e => sendControl({test_pattern_select: e.target.value}));
  $('still').addEventListener('click', () => {
    $('capmsg').textContent = 'encoding full-resolution still…';
    fetch('/still.jpg?t=' + Date.now()).then(r => {
      if (!r.ok) return r.json().then(j => { $('capmsg').textContent = 'error: ' + (j.error || r.status); });
      const name = r.headers.get('X-File');
      $('capmsg').textContent = 'saved ' + name;
      return r.blob().then(b => window.open(URL.createObjectURL(b), '_blank')).then(refreshFiles);
    }).catch(e => { $('capmsg').textContent = 'error: ' + e; });
  });
  $('capture').addEventListener('click', () => {
    post('/capture', {}).then(j => { $('capmsg').textContent = j.ok ? ('raw dump requested: ' + j.path) : ('error: ' + j.error); setTimeout(refreshFiles, 1500); })
      .catch(e => { $('capmsg').textContent = 'error: ' + e; });
  });
  $('snap').addEventListener('click', () => window.open('/snapshot.jpg?t=' + Date.now(), '_blank'));

  function refreshStatus() {
    fetch('/status.json').then(r => r.json()).then(s => {
      $('ident').textContent = (s.camera || '') + '  ' + (s.mode || '') + '  ' + (s.width || '') + '×' + (s.height || '') + ' @ ' + (s.fps ? s.fps.toFixed(2) : '?') + ' fps';
      if (s.exposure_max_ms > 0) { $('exposure_ms').max = s.exposure_max_ms.toFixed(2); $('exposure_ms_n').max = s.exposure_max_ms.toFixed(2); }
      if (s.gain_max_db > 0) { $('gain_db').max = s.gain_max_db; $('gain_db_n').max = s.gain_max_db; }
      keys.forEach(k => {
        if (Date.now() - (editing[k] || 0) < 2000) return;   // don't fight the user's drag
        if (s[k] !== undefined) { $(k).value = s[k]; $(k + '_n').value = typeof s[k] === 'number' ? +s[k].toFixed(3) : s[k]; }
      });
      $('test_pattern').checked = !!s.test_pattern;
      $('test_pattern_select').value = s.test_pattern_select || 0;
      const skip = new Set(['camera', 'mode', 'width', 'height']);
      $('status').textContent = Object.keys(s).filter(k => !skip.has(k)).map(k => k.padEnd(20) + (typeof s[k] === 'number' ? (Number.isInteger(s[k]) ? s[k] : s[k].toFixed(3)) : s[k])).join('\n');
    }).catch(() => { $('status').textContent = 'status unavailable'; });
  }
  function refreshFiles() {
    fetch('/files.json').then(r => r.json()).then(list => {
      $('files').innerHTML = '';
      list.slice(0, 30).forEach(f => {
        const a = document.createElement('a'); a.href = '/files/' + encodeURIComponent(f.name); a.target = '_blank';
        a.textContent = f.name + '  (' + (f.size / 1048576).toFixed(1) + ' MB)'; $('files').appendChild(a);
      });
    }).catch(() => {});
  }
  refreshStatus(); refreshFiles();
  setInterval(refreshStatus, 1000);
  setInterval(refreshFiles, 10000);
})();
</script>
</body>
</html>
)HTML";
}  // namespace

std::string_view PreviewPageHtml() { return kPage; }

}  // namespace hsb::preview
