#include "web_assets.hpp"

namespace kiko {

std::string_view web_index_html() {
  static constexpr std::string_view html = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>kiko web</title>
  <style>
    :root { color-scheme: light dark; --bg: #f6f7f9; --panel: #ffffff; --text: #17202a; --muted: #697386; --line: #d8dee8; --accent: #0f766e; --danger: #b42318; }
    @media (prefers-color-scheme: dark) {
      :root { --bg: #111418; --panel: #181d23; --text: #e8edf3; --muted: #9aa6b2; --line: #2b333d; --accent: #2dd4bf; --danger: #ff7b72; }
    }
    * { box-sizing: border-box; }
    body { margin: 0; background: var(--bg); color: var(--text); font: 14px/1.45 system-ui, -apple-system, Segoe UI, sans-serif; }
    header { display: flex; align-items: center; justify-content: space-between; padding: 16px 20px; border-bottom: 1px solid var(--line); }
    h1 { margin: 0; font-size: 18px; }
    main { display: grid; grid-template-columns: 360px 1fr; gap: 16px; padding: 16px; max-width: 1260px; margin: 0 auto; }
    section { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 14px; }
    h2 { margin: 0 0 10px; font-size: 15px; }
    label { display: block; margin: 10px 0 4px; color: var(--muted); font-size: 12px; }
    input, select, textarea, button { width: 100%; border: 1px solid var(--line); border-radius: 6px; padding: 8px 9px; background: transparent; color: var(--text); font: inherit; }
    textarea { min-height: 230px; resize: vertical; font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; line-height: 1.45; }
    button { cursor: pointer; background: var(--accent); color: white; border-color: var(--accent); font-weight: 600; }
    button.secondary { background: transparent; color: var(--text); border-color: var(--line); }
    button.danger { background: var(--danger); border-color: var(--danger); color: white; }
    button.inline { width: auto; padding: 6px 8px; font-size: 12px; }
    button:disabled { opacity: .55; cursor: not-allowed; }
    .tabs { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; margin-bottom: 12px; }
    .tabs button { background: transparent; color: var(--text); border-color: var(--line); }
    .tabs button.active { background: var(--accent); color: white; border-color: var(--accent); }
    .row { display: grid; grid-template-columns: 1fr 110px; gap: 8px; align-items: end; }
    .checks { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin: 10px 0; }
    .checks label { display: flex; align-items: center; gap: 7px; margin: 0; color: var(--text); }
    .checks input { width: auto; }
    .hidden { display: none; }
    details { margin-top: 12px; border-top: 1px solid var(--line); padding-top: 10px; }
    summary { cursor: pointer; color: var(--muted); font-weight: 600; }
    .status-grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
    .metric { border: 1px solid var(--line); border-radius: 8px; padding: 10px; min-height: 58px; }
    .metric .k { color: var(--muted); font-size: 12px; }
    .metric .v { margin-top: 4px; font-weight: 700; overflow-wrap: anywhere; }
    progress { width: 100%; height: 14px; margin: 12px 0; }
    pre { white-space: pre-wrap; overflow-wrap: anywhere; background: color-mix(in srgb, var(--panel), var(--line) 25%); border: 1px solid var(--line); border-radius: 8px; padding: 10px; min-height: 120px; max-height: 360px; overflow: auto; }
    .browser { margin-top: 8px; border: 1px solid var(--line); border-radius: 8px; overflow: hidden; }
    .browser-head { display: grid; grid-template-columns: 1fr 96px auto auto; gap: 8px; padding: 8px; border-bottom: 1px solid var(--line); }
    .browser-shortcuts { display: flex; flex-wrap: wrap; gap: 6px; padding: 8px; border-bottom: 1px solid var(--line); }
    .entries { max-height: 260px; overflow: auto; }
    .entry { display: grid; grid-template-columns: 1fr auto; gap: 8px; padding: 8px 10px; border-bottom: 1px solid var(--line); cursor: pointer; }
    .entry:last-child { border-bottom: 0; }
    .entry:hover { background: color-mix(in srgb, var(--accent), transparent 88%); }
    .entry .meta { color: var(--muted); font-size: 12px; }
    .muted { color: var(--muted); }
    .error { color: var(--danger); }
    .result { margin-top: 8px; font-weight: 700; }
    .hint { margin-top: 8px; border: 1px solid var(--line); border-radius: 8px; padding: 8px; }
    @media (max-width: 820px) { main { grid-template-columns: 1fr; } .status-grid { grid-template-columns: 1fr 1fr; } .browser-head { grid-template-columns: 1fr 96px; } }
  </style>
</head>
<body>
  <header>
    <h1>kiko web</h1>
    <div id="server" class="muted">local console</div>
  </header>
  <main>
    <section>
      <div class="tabs">
        <button id="tab-send" class="active" onclick="showTab('send')">Send</button>
        <button id="tab-recv" onclick="showTab('recv')">Recv</button>
        <button id="tab-note" onclick="showTab('note')">Note</button>
        <button id="tab-doctor" onclick="showTab('doctor')">Doctor</button>
      </div>

      <div id="panel-send">
        <h2>Send</h2>
        <label>Path</label>
        <div class="row"><input id="send-path"><button class="secondary" onclick="browse('send-path','file_or_dir')">Browse</button></div>
        <label>Code</label><input id="send-code" placeholder="leave empty to generate">
        <label>Relay</label><input id="send-relay">
        <details>
          <summary>Advanced network options</summary>
          <div class="checks">
            <label><input id="send-no-direct" type="checkbox"> no direct</label>
            <label><input id="send-udp-probe" type="checkbox"> UDP probe</label>
            <label><input id="send-avoid-vpn" type="checkbox"> avoid VPN</label>
            <label><input id="send-auto-connections" type="checkbox"> auto connections</label>
          </div>
        </details>
        <button id="send-start" onclick="startSend()">Start send</button>
      </div>

      <div id="panel-recv" class="hidden">
        <h2>Receive</h2>
        <label>Code</label><input id="recv-code">
        <label>Output directory</label>
        <div class="row"><input id="recv-out"><button class="secondary" onclick="browse('recv-out','dir')">Browse</button></div>
        <label>Relay</label><input id="recv-relay">
        <label>Conflict policy</label>
        <select id="recv-conflict"><option>overwrite</option><option>skip</option><option>rename</option></select>
        <details>
          <summary>Advanced network options</summary>
          <div class="checks">
            <label><input id="recv-no-direct" type="checkbox"> no direct</label>
            <label><input id="recv-udp-probe" type="checkbox"> UDP probe</label>
            <label><input id="recv-avoid-vpn" type="checkbox"> avoid VPN</label>
          </div>
        </details>
        <button id="recv-start" onclick="startRecv()">Start receive</button>
      </div>

      <div id="panel-doctor" class="hidden">
        <h2>Doctor</h2>
        <label>Relay</label><input id="doctor-relay">
        <details>
          <summary>Advanced network options</summary>
          <div class="checks">
            <label><input id="doctor-udp-probe" type="checkbox"> UDP probe</label>
            <label><input id="doctor-avoid-vpn" type="checkbox"> avoid VPN</label>
          </div>
        </details>
        <button id="doctor-start" onclick="startDoctor()">Run doctor</button>
      </div>

      <div id="panel-note" class="hidden">
        <h2>Notepad</h2>
        <label>Role</label>
        <select id="note-role"><option value="host">host</option><option value="join">join</option></select>
        <label>Code</label><input id="note-code" placeholder="leave empty to generate when hosting">
        <label>Relay</label><input id="note-relay">
        <details>
          <summary>Advanced network options</summary>
          <div class="checks">
            <label><input id="note-no-direct" type="checkbox"> no direct</label>
            <label><input id="note-lan" type="checkbox" checked> LAN discovery</label>
            <label><input id="note-local-relay" type="checkbox" checked> LAN relay</label>
            <label><input id="note-udp-probe" type="checkbox"> UDP probe</label>
            <label><input id="note-avoid-vpn" type="checkbox"> avoid VPN</label>
          </div>
        </details>
        <button id="note-start" onclick="startNote()">Start notepad</button>
        <label>Shared note</label>
        <textarea id="note-text" placeholder="Start the notepad, then type here..." disabled oninput="queueNoteUpdate()"></textarea>
        <div class="row" style="margin-top:8px">
          <div id="note-meta" class="muted">not running</div>
          <button id="note-clear" class="secondary" onclick="clearNote()" disabled>Clear</button>
        </div>
      </div>

      <div id="browser" class="browser hidden">
        <div class="browser-head">
          <input id="browser-filter" placeholder="filter" oninput="loadBrowser()">
          <select id="browser-sort" onchange="loadBrowser()"><option value="name">name</option><option value="modified">modified</option></select>
          <button class="secondary inline" type="button" onclick="clearBrowserFilter()">Clear</button>
          <button class="secondary inline" type="button" onclick="closeBrowser()">Close</button>
        </div>
        <div id="browser-shortcuts" class="browser-shortcuts"></div>
        <div class="muted" style="padding:8px" id="browser-path"></div>
        <div id="browser-entries" class="entries"></div>
      </div>
    </section>

    <section>
      <h2>Status</h2>
      <div class="status-grid">
        <div class="metric"><div class="k">task</div><div id="m-kind" class="v">idle</div></div>
        <div class="metric"><div class="k">stage</div><div id="m-activity" class="v">idle</div></div>
        <div class="metric"><div class="k">code</div><div id="m-code" class="v">-</div></div>
        <div class="metric"><div class="k">route</div><div id="m-route" class="v">-</div></div>
      </div>
      <progress id="overall" max="100" value="0"></progress>
      <div class="row">
        <div class="muted" id="progress-text">0 B / 0 B</div>
        <button class="danger" id="cancel" onclick="cancelJob()" disabled>Cancel</button>
      </div>
      <div id="result-text" class="result muted"></div>
      <div id="error-hint" class="hint error hidden"></div>
      <label>Current file</label><div id="current-file" class="muted">-</div>
      <label>Timing</label><div id="timing" class="muted">-</div>
      <label>Log</label><pre id="log"></pre>
      <label>Doctor JSON</label><pre id="doctor-json"></pre>
    </section>
  </main>
  <script>
    const token = new URLSearchParams(location.search).get('token') || '';
    let browseTarget = null;
    let browseMode = 'file_or_dir';
    let browsePath = '';
    let poll = null;
    let webConfig = {};
    let noteDirty = false;
    let noteApplying = false;
    let noteUpdateTimer = null;
    let noteEditGeneration = 0;

    function api(path, options = {}) {
      const url = path + (path.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(token);
      return fetch(url, Object.assign({headers: {'content-type': 'application/json'}}, options))
        .then(async r => {
          const text = await r.text();
          const body = text ? JSON.parse(text) : {};
          if (!r.ok) throw new Error(body.error || r.statusText);
          return body;
        });
    }
    function qs(id) { return document.getElementById(id); }
    function bytes(n) {
      const u = ['B','KB','MB','GB','TB']; let v = Number(n || 0), i = 0;
      while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
      return (i ? v.toFixed(v < 10 ? 2 : v < 100 ? 1 : 0) : String(Math.round(v))) + ' ' + u[i];
    }
    function duration(ms) {
      const s = Math.round(Number(ms || 0) / 1000);
      if (s < 60) return s + 's';
      const m = Math.floor(s / 60), r = s % 60;
      return m + 'm ' + String(r).padStart(2, '0') + 's';
    }
    function storageGet(key) {
      try { return localStorage.getItem(key) || ''; } catch (_) { return ''; }
    }
    function storageSet(key, value) {
      try {
        if (value) localStorage.setItem(key, value);
      } catch (_) {}
    }
    function storedPath(target) { return storageGet('kiko.web.path.' + target); }
    function storedBrowserPath(target) { return storageGet('kiko.web.browser.' + target); }
    function rememberPath(target, path) {
      if (!path) return;
      storageSet('kiko.web.path.' + target, path);
      if (qs(target)) qs(target).value = path;
    }
    function rememberBrowserPath(target, path) {
      if (!target || !path) return;
      storageSet('kiko.web.browser.' + target, path);
    }
    function friendlyError(message) {
      const m = String(message || '').toLowerCase();
      if (!m) return '';
      if (m.includes('already running')) return 'A transfer is already running. Cancel it or wait for it to finish before starting another task.';
      if (m.includes('invalid character') || m.includes('pairing code')) return 'Check the pairing code. Use the exact code shown on the other device; ambiguous characters may be rejected.';
      if (m.includes('expired') || m.includes('pair timeout') || m.includes('room') || m.includes('peer')) return 'The pairing window may have expired. Start the sender again, then enter the fresh code promptly.';
      if (m.includes('relay') || m.includes('connection refused') || m.includes('timed out')) return 'Check the relay address, network reachability, VPN/proxy settings, and whether the peer is still waiting with the same code.';
      if (m.includes('permission') || m.includes('denied')) return 'Check file or output directory permissions. Try a folder under your home or Downloads directory.';
      if (m.includes('not a directory')) return 'Pick an existing output directory.';
      if (m.includes('note code is required')) return 'Choose Join only after entering the host notepad code.';
      if (m.includes('path is required')) return 'Choose a file or folder before starting send.';
      if (m.includes('code is required')) return 'Enter the pairing code shown on the other device.';
      return 'Open the log below for details, then retry after checking the code, relay, and selected paths.';
    }
    function showHint(message) {
      qs('error-hint').textContent = message;
      qs('error-hint').classList.toggle('hidden', !message);
      if (message) qs('log').textContent += '\nERROR: ' + message;
    }
    function clearHint() {
      qs('error-hint').textContent = '';
      qs('error-hint').classList.add('hidden');
    }
    function setRunningButtons(running) {
      for (const id of ['send-start', 'recv-start', 'doctor-start', 'note-start']) qs(id).disabled = running;
    }
    function showTab(name) {
      for (const n of ['send','recv','note','doctor']) {
        qs('panel-' + n).classList.toggle('hidden', n !== name);
        qs('tab-' + n).classList.toggle('active', n === name);
      }
    }
    function startPolling() {
      if (poll) return;
      poll = setInterval(loadJob, 250);
      loadJob();
    }
    function loadConfig() {
      api('/api/config').then(c => {
        webConfig = c;
        qs('server').textContent = c.listen || 'local console';
        qs('send-relay').value = c.relay || '';
        qs('recv-relay').value = c.relay || '';
        qs('note-relay').value = c.relay || '';
        qs('doctor-relay').value = c.relay || '';
        qs('send-path').value = storedPath('send-path') || c.last_send_path || '';
        qs('recv-out').value = storedPath('recv-out') || c.last_recv_out_dir || '.';
        qs('note-lan').checked = c.network ? !!c.network.lan_discover : true;
        qs('note-no-direct').checked = c.network ? !!c.network.no_direct : false;
        qs('note-udp-probe').checked = c.network ? !!c.network.udp_probe : false;
        qs('note-avoid-vpn').checked = c.network ? !!c.network.avoid_vpn : false;
        renderShortcuts();
      }).catch(showError);
    }
    function startSend() {
      clearHint();
      if (!qs('send-path').value.trim()) {
        showHint('Choose a file or folder before starting send.');
        return;
      }
      rememberPath('send-path', qs('send-path').value.trim());
      const body = {
        path: qs('send-path').value, code: qs('send-code').value, relay: qs('send-relay').value,
        no_direct: qs('send-no-direct').checked, udp_probe: qs('send-udp-probe').checked,
        avoid_vpn: qs('send-avoid-vpn').checked, auto_connections: qs('send-auto-connections').checked
      };
      api('/api/send', {method:'POST', body: JSON.stringify(body)}).then(startPolling).catch(showError);
    }
    function startRecv() {
      clearHint();
      if (!qs('recv-code').value.trim()) {
        showHint('Enter the pairing code shown on the other device.');
        return;
      }
      if (!qs('recv-out').value.trim()) {
        showHint('Choose an output directory.');
        return;
      }
      rememberPath('recv-out', qs('recv-out').value.trim());
      const body = {
        code: qs('recv-code').value, out: qs('recv-out').value, relay: qs('recv-relay').value,
        on_conflict: qs('recv-conflict').value, no_direct: qs('recv-no-direct').checked,
        udp_probe: qs('recv-udp-probe').checked, avoid_vpn: qs('recv-avoid-vpn').checked
      };
      api('/api/recv', {method:'POST', body: JSON.stringify(body)}).then(startPolling).catch(showError);
    }
    function startDoctor() {
      clearHint();
      const body = { relay: qs('doctor-relay').value, udp_probe: qs('doctor-udp-probe').checked, avoid_vpn: qs('doctor-avoid-vpn').checked };
      api('/api/doctor', {method:'POST', body: JSON.stringify(body)}).then(startPolling).catch(showError);
    }
    function startNote() {
      clearHint();
      if (qs('note-role').value === 'join' && !qs('note-code').value.trim()) {
        showHint('Choose Join only after entering the host notepad code.');
        return;
      }
      const body = {
        role: qs('note-role').value, code: qs('note-code').value, relay: qs('note-relay').value,
        no_direct: qs('note-no-direct').checked, no_lan: !qs('note-lan').checked,
        no_local: !qs('note-local-relay').checked, udp_probe: qs('note-udp-probe').checked,
        avoid_vpn: qs('note-avoid-vpn').checked
      };
      api('/api/note/start', {method:'POST', body: JSON.stringify(body)}).then(() => {
        noteDirty = false;
        noteEditGeneration = 0;
        qs('note-text').disabled = false;
        qs('note-clear').disabled = false;
        startPolling();
      }).catch(showError);
    }
    function queueNoteUpdate() {
      if (noteApplying) return;
      noteDirty = true;
      noteEditGeneration += 1;
      if (noteUpdateTimer) clearTimeout(noteUpdateTimer);
      noteUpdateTimer = setTimeout(sendNoteUpdate, 250);
    }
    function sendNoteUpdate() {
      noteUpdateTimer = null;
      const generation = noteEditGeneration;
      const text = qs('note-text').value;
      api('/api/note/update', {method:'POST', body: JSON.stringify({text})})
        .then(() => {
          if (generation === noteEditGeneration) noteDirty = false;
          loadJob();
        })
        .catch(showError);
    }
    function clearNote() {
      clearHint();
      if (noteUpdateTimer) { clearTimeout(noteUpdateTimer); noteUpdateTimer = null; }
      noteEditGeneration += 1;
      noteDirty = false;
      noteApplying = true;
      qs('note-text').value = '';
      noteApplying = false;
      api('/api/note/clear', {method:'POST', body:'{}'}).then(loadJob).catch(showError);
    }
    function cancelJob() {
      qs('cancel').disabled = true;
      qs('cancel').textContent = 'Canceling...';
      qs('m-activity').textContent = 'cancel requested';
      qs('result-text').textContent = 'Cancel requested...';
      api('/api/job/cancel', {method:'POST', body:'{}'})
        .then(() => { startPolling(); loadJob(); })
        .catch(showError);
    }
    function loadJob() {
      api('/api/job').then(j => {
        const terminal = !!j.terminal || (!j.running && (j.finished || j.failed || j.canceled));
        qs('m-kind').textContent = j.kind || 'idle';
        qs('m-activity').textContent = terminal ? terminalActivity(j) : (j.activity || (j.running ? 'running' : 'idle'));
        qs('m-code').textContent = j.code || '-';
        qs('m-route').textContent = j.route_summary || j.route_phase || '-';
        qs('current-file').textContent = j.current_file || '-';
        qs('timing').textContent = j.route_timing || (j.elapsed_ms ? (j.elapsed_ms + 'ms') : '-');
        qs('log').textContent = (j.logs || []).join('\n') + (j.error ? '\nERROR: ' + j.error : '');
        qs('doctor-json').textContent = j.doctor_json || '';
        const total = Number(j.overall_total || 0), done = Number(j.overall_done || 0);
        qs('overall').value = total > 0 ? Math.min(100, done * 100 / total) : (j.finished ? 100 : 0);
        const speed = Number(j.average_bytes_per_sec || 0);
        const speedLabel = speed ? (terminal ? ' · avg ' : ' · ') + bytes(speed) + '/s' : '';
        qs('progress-text').textContent = bytes(done) + ' / ' + bytes(total) + speedLabel;
        qs('result-text').textContent = terminal ? resultText(j) : '';
        const hint = j.error ? friendlyError(j.error) : '';
        qs('error-hint').textContent = hint;
        qs('error-hint').classList.toggle('hidden', !hint);
        qs('cancel').disabled = !j.running;
        qs('cancel').textContent = j.running && j.activity === 'cancel requested' ? 'Canceling...' : 'Cancel';
        if (j.activity === 'cancel requested') qs('cancel').disabled = true;
        setRunningButtons(j.running);
        updateNotePanel(j);
        if (terminal && poll) { clearInterval(poll); poll = null; }
      }).catch(e => { if (poll) { clearInterval(poll); poll = null; } showError(e); });
    }
    function terminalActivity(j) {
      if (j.canceled) return 'canceled';
      if (j.failed) return 'failed';
      if (j.kind === 'doctor') return 'doctor complete';
      if (j.kind === 'note') return j.activity || 'notepad closed';
      return j.activity || 'complete';
    }
    function updateNotePanel(j) {
      const isNote = j.kind === 'note' && (j.running || j.finished || j.note_revision || j.note_text);
      const editable = isNote && j.running && !j.failed && !j.canceled;
      qs('note-text').disabled = !editable;
      qs('note-clear').disabled = !editable;
      if (isNote && j.code && qs('note-role').value === 'host' && !qs('note-code').value.trim()) {
        qs('note-code').value = j.code;
      }
      if (isNote && !noteDirty && qs('note-text').value !== (j.note_text || '')) {
        noteApplying = true;
        qs('note-text').value = j.note_text || '';
        noteApplying = false;
      }
      if (isNote) {
        const sync = j.note_synced ? 'synced' : (j.running ? (j.note_connected ? 'connected' : 'connecting') : 'closed');
        qs('note-meta').textContent = 'rev ' + Number(j.note_revision || 0) + ' · ' + sync;
      } else {
        qs('note-meta').textContent = 'not running';
      }
    }
    function resultText(j) {
      if (j.canceled) return 'Canceled after ' + duration(j.elapsed_ms || 0) + '.';
      if (j.failed) return 'Failed after ' + duration(j.elapsed_ms || 0) + '.';
      if (j.kind === 'doctor') return 'Doctor finished in ' + duration(j.elapsed_ms || 0) + '.';
      if (j.kind === 'note') return 'Notepad closed after ' + duration(j.elapsed_ms || 0) + '.';
      const speed = Number(j.average_bytes_per_sec || 0);
      const files = Number(j.files_total || j.files_done || 0);
      return 'Complete: ' + bytes(j.overall_done || 0) + (files ? ' across ' + files + ' file(s)' : '') +
             ' in ' + duration(j.elapsed_ms || 0) + (speed ? ' · avg ' + bytes(speed) + '/s' : '') + '.';
    }
    function browse(target, mode) {
      browseTarget = target;
      browseMode = mode;
      browsePath = storedBrowserPath(target) || qs(target).value || storedPath(target) || '.';
      qs('browser').classList.remove('hidden');
      renderShortcuts();
      loadBrowser();
    }
    function closeBrowser() {
      browseTarget = null;
      qs('browser').classList.add('hidden');
    }
    function clearBrowserFilter() {
      qs('browser-filter').value = '';
      loadBrowser();
    }
    function renderShortcuts() {
      const root = qs('browser-shortcuts');
      root.innerHTML = '';
      for (const s of webConfig.shortcuts || []) {
        const btn = document.createElement('button');
        btn.className = 'secondary inline';
        btn.type = 'button';
        btn.textContent = s.label;
        btn.onclick = () => {
          browsePath = s.path;
          rememberBrowserPath(browseTarget, browsePath);
          loadBrowser();
        };
        root.appendChild(btn);
      }
    }
    function loadBrowser() {
      if (!browseTarget) return;
      const q = new URLSearchParams({ path: browsePath, mode: browseMode, sort: qs('browser-sort').value, filter: qs('browser-filter').value });
      api('/api/fs?' + q.toString()).then(b => {
        browsePath = b.path;
        rememberBrowserPath(browseTarget, browsePath);
        qs('browser-path').textContent = b.path;
        const root = qs('browser-entries');
        root.innerHTML = '';
        for (const e of b.entries || []) {
          const div = document.createElement('div');
          div.className = 'entry';
          div.innerHTML = '<span></span><span class="meta"></span>';
          div.children[0].textContent = e.label;
          div.children[1].textContent = e.is_dir ? 'dir' : 'file';
          div.onclick = () => {
            if (e.parent || (e.is_dir && !e.select_here)) {
              browsePath = e.path;
              rememberBrowserPath(browseTarget, browsePath);
              loadBrowser();
              return;
            }
            if (e.selectable) {
              rememberPath(browseTarget, e.path);
              rememberBrowserPath(browseTarget, e.is_dir ? e.path : browsePath);
              qs('browser').classList.add('hidden');
              return;
            }
            if (e.is_dir) {
              browsePath = e.path;
              rememberBrowserPath(browseTarget, browsePath);
              loadBrowser();
            }
          };
          root.appendChild(div);
        }
      }).catch(showError);
    }
    function showError(e) {
      const message = e && e.message ? e.message : e;
      qs('log').textContent += '\nERROR: ' + message;
      qs('error-hint').textContent = friendlyError(message);
      qs('error-hint').classList.remove('hidden');
    }
    loadConfig();
    loadJob();
  </script>
</body>
</html>)HTML";
  return html;
}

}  // namespace kiko
