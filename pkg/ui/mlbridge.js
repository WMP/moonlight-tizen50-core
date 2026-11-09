/* eslint-disable */
/*
 * mlbridge.js - transport shim for the Moonlight Tizen 5.0 web UI.
 *
 * moonlight-tizen's UI (platform/messages.js) calls the native WASM module via
 * `Module.makeCert/httpInit/openUrl/pair/startStream/...`. On Tizen 5.0 there is
 * no WASM/EMSS; the native work runs in the .NET host
 * (org.moonlight.host.app), reachable over the "moonlight" message port. The host is
 * a no-display UI app in its own .tpk: the UI starts it and connects to its port.
 * This file replaces the Module-based transport with message-port calls, keeping
 * messages.js's sendMessage() contract. Wire format: see pkg/ui/PROTOCOL.md.
 *
 * Load this AFTER platform/messages.js so it overrides SyncFunctions/AsyncFunctions
 * and sendMessage. Host-side test: tests/mlbridge_test.js.
 */
(function () {
  var ENGINE_APPID = 'org.moonlight.host.app';
  var ENGINE_PORT = 'moonlight';
  var LOCAL_PORT = 'moonlightui';

  var TRACE = !!window.ML_TRACE;   // set by mltrace.js in debug builds
  function trace() {
    if (!TRACE) return;
    var a = ['[mlbridge]'].concat(Array.prototype.slice.call(arguments));
    console.log.apply(console, a);
  }

  // index.js reads Samsung product/AV info at top level (index.js:2-29). One throwing
  // call there aborts the whole file (db, BUILD_TYPE never initialise) and the UI
  // resets to the splash. mlbridge.js loads before index.js, so wrap those calls
  // to log and return a safe default instead of throwing.
  function guard(obj, name, names, dflt) {
    if (!obj) return;
    names.forEach(function (n) {
      var f = obj[n];
      if (typeof f !== 'function') return;
      obj[n] = function () {
        try { return f.apply(obj, arguments); }
        catch (e) { console.warn('[mlbridge] ' + name + '.' + n + ' failed, using default:', e && (e.message || e.name || e)); return dflt; }
      };
    });
  }
  try {
    if (window.webapis) {
      guard(webapis.productinfo, 'productinfo', ['getModel', 'getRealModel', 'getModelCode', 'getFirmware', 'getDuid'], '');
      guard(webapis.productinfo, 'productinfo', ['isUdPanelSupported', 'is8KPanelSupported'], false);
      guard(webapis.avinfo, 'avinfo', ['isHdrTvSupport'], false);
    }
  } catch (e) { console.error('[mlbridge] webapis guard failed:', e); }

  // index.js logs stored values, including the pairing private key ("cert" in the DB).
  // Mask PEM private keys in every console line (inspector, debug relay to dlog).
  // mlbridge.js loads before index.js, so this wrapper sees all of its logging.
  var PEM_KEY = /-----BEGIN ([A-Z ]*PRIVATE KEY)-----[\s\S]*?-----END \1-----/g;
  function redact(v) { return typeof v === 'string' ? v.replace(PEM_KEY, '-----BEGIN $1----- [redacted] -----END $1-----') : v; }
  ['log', 'info', 'warn', 'error', 'debug'].forEach(function (lvl) {
    var orig = console[lvl];
    if (typeof orig !== 'function') return;
    console[lvl] = function () {
      var a = [];
      for (var i = 0; i < arguments.length; i++) a.push(redact(arguments[i]));
      return orig.apply(console, a);
    };
  });

  var localPort = null, remotePort = null, nextId = 1;
  var pending = {};   // id -> {resolve, reject, timer, msg, cmd, timeoutMs, cancelOnTimeout}
  var queue = [];     // ids waiting for the remote port, in send order
  var retryTimer = null, retryDelay = 250;
  var DEFAULT_TIMEOUT = 15000, PAIR_TIMEOUT = 120000, RETRY_MAX = 4000;

  function errText(e) {
    if (!e) return '?';
    var p = [e.name, e.message, e.code].filter(function (x) { return x !== undefined && x !== null && x !== ''; });
    return p.length ? p.join(' / ') : String(e);
  }

  // Once, on the first miss: say in the console whether the host is installed and
  // running, so a UI log alone tells "not installed" from "installed, not started".
  var hostReported = false;
  function reportHost() {
    if (hostReported) return;
    hostReported = true;
    var info;
    try { info = tizen.application.getAppInfo(ENGINE_APPID); }
    catch (e) { console.warn('[mlbridge] host ' + ENGINE_APPID + ' is not installed (' + (e && (e.name || e.message || e)) + ')'); return; }
    var what = '[mlbridge] host ' + ENGINE_APPID + ' installed (version ' + info.version + ', package ' + info.packageId + ')';
    trace('host app info: name=' + info.name + ' show=' + info.show + ' installed=' + info.installDate + ' size=' + info.size);
    try {
      tizen.application.getAppsContext(function (ctx) {
        var up = ctx.some(function (c) { return c.appId === ENGINE_APPID; });
        console.warn(what + (up ? ', running, but its port is not open' : ', not running'));
      }, function (e) { console.warn(what + ', running state unknown (' + (e && (e.name || e.message || e)) + ')'); });
    } catch (e) { console.warn(what + ', running state unknown (' + (e && (e.name || e.message || e)) + ')'); }
  }

  // Whether the host process runs and its port is open; calls done(running).
  function hostState(tag, done) {
    var port = 'open';
    try { tizen.messageport.requestRemoteMessagePort(ENGINE_APPID, ENGINE_PORT); }
    catch (e) { port = 'missing (' + errText(e) + ')'; }
    function say(proc, up) { console.log('[mldiag] ' + tag + ': host process ' + proc + ', port ' + port); if (done) done(up); }
    try {
      tizen.application.getAppsContext(function (ctx) {
        var c = ctx.filter(function (x) { return x.appId === ENGINE_APPID; });
        say(c.length ? 'running (context ' + c[0].id + ')' : 'not running', c.length > 0);
      }, function (e) { say('state unknown (' + errText(e) + ')', false); });
    } catch (e) { say('state unknown (' + errText(e) + ')', false); }
  }

  // Start the host (a no-display UI app) on the first miss, then every LAUNCH_EVERY-th.
  var LAUNCH_EVERY = 8, misses = 0, starts = 0, uiStart = Date.now();
  function startHost() {
    var n = ++starts, at = Date.now();
    console.log('[mlbridge] host start #' + n + ': tizen.application.launch(' + ENGINE_APPID + '), ' + (at - uiStart) + ' ms after UI start');
    try {
      tizen.application.launch(ENGINE_APPID,
        function () {
          console.log('[mlbridge] host start #' + n + ' requested (accepted in ' + (Date.now() - at) + ' ms)');
          if (TRACE) setTimeout(function () { hostState('3 s after host start #' + n); }, 3000);
        },
        function (e) { console.error('[mlbridge] host start #' + n + ' failed: ' + errText(e)); });
    } catch (e) { console.error('[mlbridge] host start #' + n + ' failed (threw): ' + errText(e)); }
  }

  function ensurePorts() {
    if (localPort && remotePort) return true;
    trace('ensurePorts: connecting', ENGINE_APPID, ENGINE_PORT);
    try {
      if (!localPort) {
        var lp = tizen.messageport.requestLocalMessagePort(LOCAL_PORT);
        lp.addMessagePortListener(onReply);
        localPort = lp;   // set only once the listener is attached
      }
    } catch (e) {
      console.error('[mlbridge] local message port setup failed:', e);
      return false;
    }
    try {
      remotePort = tizen.messageport.requestRemoteMessagePort(ENGINE_APPID, ENGINE_PORT);
      misses = 0;   // connected: a later loss starts the host again at once
      trace('ensurePorts: ok');
      return true;
    } catch (e) {
      trace('ensurePorts: remote port missing (miss ' + (misses + 1) + ')', errText(e));
      reportHost();
      if (misses++ % LAUNCH_EVERY === 0) startHost();
      return false;
    }
  }

  // Tizen message ports reject a message over ~4 KB, so a large message is sent as
  // ordered parts {id, seq, total, field, chunk} (pkg/ui/PROTOCOL.md). The limits
  // leave room for the Bundle encoding overhead.
  var MAX_SINGLE = 1536, CHUNK_CHARS = 512;   // 512 chars <= 1536 UTF-8 bytes
  function utf8Len(v) { try { return unescape(encodeURIComponent(v)).length; } catch (e) { return v.length * 3; } }

  // msg: [{key,value}] with cmd and id first. Returns the list of messages to send.
  function split(msg) {
    var size = 0, i;
    for (i = 0; i < msg.length; i++) size += msg[i].key.length + utf8Len(msg[i].value);
    if (size <= MAX_SINGLE) return [msg];
    var parts = [];
    for (i = 2; i < msg.length; i++) {
      var v = msg[i].value, off = 0;
      do { parts.push({ field: msg[i].key, chunk: v.substr(off, CHUNK_CHARS) }); off += CHUNK_CHARS; } while (off < v.length);
    }
    return parts.map(function (pt, seq) {
      return [msg[0], msg[1], { key: 'seq', value: String(seq) }, { key: 'total', value: String(parts.length) },
              { key: 'field', value: pt.field }, { key: 'chunk', value: pt.chunk }];
    });
  }

  // Collects the parts of a chunked reply; returns the joined map once all arrived.
  var partial = {};   // id -> {total, n, seqs: {seq: {field, chunk}}}
  var AU_PARTIAL_MAX = 8, auOrder = [];   // pending "au" ids, oldest first: a frame that lost a part must not stay
  function isAu(id) { return String(id).indexOf('au') === 0; }
  function clearAuPartials() { auOrder.forEach(function (id) { delete partial[id]; }); auOrder = []; }
  function join(m) {
    var au = isAu(m.id);
    if (au && !partial[m.id]) {
      auOrder.push(m.id);
      while (auOrder.length > AU_PARTIAL_MAX) delete partial[auOrder.shift()];
    }
    var a = partial[m.id] || (partial[m.id] = { total: +m.total, n: 0, seqs: {} });
    if (!a.seqs[m.seq]) { a.seqs[m.seq] = { field: m.field, chunk: m.chunk }; a.n++; }
    if (a.n < a.total) return null;
    delete partial[m.id];
    if (au) { var k = auOrder.indexOf(m.id); if (k >= 0) auOrder.splice(k, 1); }
    var out = { id: m.id };
    if (au) {   // binary chunks: one Uint8Array, no concat per frame
      var tl = 0, i0, c0;
      for (i0 = 0; i0 < a.total; i0++) { c0 = a.seqs[i0]; if (c0 && c0.field === 'data' && typeof c0.chunk !== 'string') tl += c0.chunk.length; }
      var bin = new Uint8Array(tl), off = 0;
      for (i0 = 0; i0 < a.total; i0++) {
        c0 = a.seqs[i0];
        if (!c0) continue;
        if (c0.field === 'data' && typeof c0.chunk !== 'string') { bin.set(c0.chunk, off); off += c0.chunk.length; }
        else out[c0.field] = (out[c0.field] || '') + c0.chunk;
      }
      out.data = bin;
      return out;
    }
    for (var i = 0; i < a.total; i++) {
      var pt = a.seqs[i], prev = out[pt.field];
      if (prev === undefined) out[pt.field] = pt.chunk;
      else if (typeof prev === 'string') out[pt.field] = prev + pt.chunk;
      else out[pt.field] = prev.concat(Array.prototype.slice.call(pt.chunk));   // byte chunks
    }
    if (!au) trace('joined id=' + m.id, 'parts=' + a.total);
    return out;
  }

  // Send one pending command. Its timeout is armed only after sendMessage succeeded,
  // so time spent waiting for the host to start does not count against it.
  function sendNow(id) {
    var p = pending[id];
    if (!p) return;
    try { for (var i = 0; i < p.msgs.length; i++) remotePort.sendMessage(p.msgs[i], localPort); }
    catch (e) {   // host gone: forget the port, keep the command first in the queue, reconnect
      trace('send failed id=' + id, e && (e.name || e.message || e));
      remotePort = null;
      queue.unshift(id);
      scheduleRetry();
      return;
    }
    trace('sent cmd=' + p.cmd, 'id=' + id, p.msgs.length > 1 ? 'parts=' + p.msgs.length : '');
    if (p.timeoutMs > 0) p.timer = setTimeout(function () { onTimeout(id); }, p.timeoutMs);
  }

  function onTimeout(id) {
    var p = pending[id];
    if (!p) return;
    delete pending[id];
    trace('timeout cmd=' + p.cmd, 'id=' + id);
    if (p.cancelOnTimeout) bridgeCall('cancelRequest', {}, 0).then(null, function () {});
    p.reject('timeout');
  }

  function flush() {
    trace('flush queued=' + queue.length);
    while (queue.length && remotePort) sendNow(queue.shift());
  }

  // Connect now, or retry with backoff (250 ms doubling up to 4 s); one timer at a time.
  function connect() {
    if (retryTimer) return;
    if (ensurePorts()) { retryDelay = 250; flush(); return; }
    scheduleRetry();
  }

  function scheduleRetry() {
    if (retryTimer) return;
    trace('retry in ' + retryDelay + ' ms');
    retryTimer = setTimeout(function () {
      retryTimer = null;
      retryDelay = Math.min(retryDelay * 2, RETRY_MAX);
      connect();
    }, retryDelay);
  }

  // Live stats overlay (event mlstats, see PROTOCOL.md); shown after an odd number of toggles.
  var wantStats = false;   // startRequest a[29]: turn the overlay on at Connection Established
  var statsOn = false, statsEl = null, statsGen = 0;   // statsGen: bumped on streamTerminated
  var audioRoute = 'none';   // last mlaroute: 'native' | 'webaudio' | 'none' (see PROTOCOL.md)
  function statsDiv(create) {
    if (!statsEl && create && typeof document !== 'undefined' && document.body && document.createElement) {
      statsEl = document.createElement('div');
      statsEl.id = 'mlstats';
      statsEl.style.cssText = 'position:fixed;top:0;left:0;background:rgba(0,0,0,.55);color:#fff;' +
        'font:12px monospace;padding:6px 8px;z-index:99999;white-space:pre';
      document.body.appendChild(statsEl);
    }
    return statsEl;
  }
  function showStats(m) {
    var el = statsDiv(statsOn);
    if (!el || !statsOn) return;
    el.style.display = 'block';
    el.textContent = 'depth ' + m.depth + ' ms\n' +
      'fps ' + m.fps + '\n' +
      'dropped late ' + m.drop + ' / full ' + m.full + '\n' +
      'under/over ' + m.uo + '\n' +
      'errors ' + m.err + '  recoveries ' + m.rec + '\n' +
      'play ' + (Number(m.play) / 1000).toFixed(1) + ' s';
    if (m.aroute === 'native') {   // the engine plays audio itself (no mlaudio events)
      el.textContent += '\naudio native  buf ' + m.abuf + ' ms  frames ' + m.aframes +
        '\nunder/over ' + m.auo + '  dropped ' + m.adrops;
    } else {   // callback path: the host counters and the UI's own scheduler counters
      if (m.asent !== undefined) el.textContent += '\nhost audio sent ' + m.asent + ' drop ' + m.adrop + ' fail ' + m.afail;   // older host: absent
      if (typeof window.mlAudioStats === 'function') {   // the host does not know the UI audio scheduler
        var a = window.mlAudioStats();
        el.textContent += '\nweb audio rx ' + a.received + ' sched ' + a.scheduled + ' drop ' + a.dropped + ' empty ' + a.empty +
          '\nresync ' + a.resyncs + ' underrun ' + a.underruns + ' headroom ' + Number(a.headroomMs).toFixed(0) + ' ms';
      }
    }
  }
  // Audio scheduler (mlaudio.js); absent = no audio, never an error.
  function audioCall(fn) { if (fn === 'mlAudioStop') clearAuPartials(); if (typeof window[fn] === 'function') { try { window[fn](); } catch (e) { console.warn('[mlbridge] ' + fn + ' failed:', e); } } }
  function resetStats() {
    statsOn = false; statsGen++; audioRoute = 'none';
    if (statsEl) { statsEl.textContent = ''; statsEl.style.display = 'none'; }
  }
  // Sends the wanted state (on), so a lost reply cannot leave the host and the overlay inverted.
  function toggleStats() {
    var want = !statsOn, gen = statsGen;
    return bridgeCall('toggleStats', { on: String(want) }, DEFAULT_TIMEOUT).then(function (r) {
      if (r && r.osd !== undefined) {   // the engine lowers its video window under the UI while the overlay is on
        if (String(r.osd) === '0') console.log('[mlbridge] stats over video: osd ' + r.osd + ' window ' + r.osdWin);
        else console.warn('[mlbridge] stats over video: osd ' + r.osd + ' window ' + r.osdWin +
          ': window stacking not supported by this TV: overlay visible only after the stream');
      }
      if (gen !== statsGen) return r;   // the stream ended meanwhile: the host stopped polling
      if (!want) resetStats();
      else statsOn = true;   // the overlay appears with the first mlstats event
      return r;
    });
  }


  // --- Gamepad input: port of wasm/gamepad.cpp PollGamepads() (no WASM module polls here) ---
  // While a stream runs, navigator.getGamepads() is polled and changes go to the host as
  // gamepadArrival / gamepadState (no reply, see PROTOCOL.md). Not ported: mouse emulation,
  // rumble, touchpad/motion/battery.
  var PAD_MAX = 16, PAD_MS = 8, PAD_STICK_EPS = 327, PAD_KEEPALIVE = 32, padTraceFail = false, padTimer = null, padSlots = [], padFlipAB = false, padFlipXY = false;
  var F = { A: 0x1000, B: 0x2000, X: 0x4000, Y: 0x8000, UP: 1, DOWN: 2, LEFT: 4, RIGHT: 8, PLAY: 0x10, BACK: 0x20,
            LS: 0x40, RS: 0x80, LB: 0x100, RB: 0x200, SPECIAL: 0x400 };
  var PAD_FLAGS = [F.A, F.B, F.X, F.Y, F.LB, F.RB, 0, 0, F.BACK, F.PLAY, F.LS, F.RS, F.UP, F.DOWN, F.LEFT, F.RIGHT, F.SPECIAL];
  var STOP_COMBO = F.BACK | F.PLAY | F.LB | F.RB, STATS_COMBO = F.BACK | F.LB | F.RB | F.X;

  // Same A<->B / X<->Y swaps as the C++ tables (GetButtonFlags and GetSupportedButtons).
  function padFlag(i) {
    var f = PAD_FLAGS[i];
    if (padFlipAB && i < 2) return PAD_FLAGS[1 - i];
    if (padFlipXY && i >= 2 && i < 4) return PAD_FLAGS[5 - i];
    return f;
  }
  function padButtonFlags(g) {
    var r = 0, n = Math.min(g.buttons.length, PAD_FLAGS.length);
    for (var i = 0; i < n; i++) if (g.buttons[i].pressed) r |= padFlag(i);
    return r;
  }
  function padSupported(n) {
    var r = 0;
    for (var i = 0; i < Math.min(n, 17); i++) r |= padFlag(i);
    return r;
  }
  function padType(id) {
    var l = String(id || '').toLowerCase();
    if (!l) return 0;
    function has(x) { return l.indexOf(x) >= 0; }
    if (has('vendor: 054c') || has('vendor:054c') || l.indexOf('054c-') === 0 || has('dualsense') || has('dualshock') ||
        has('playstation') || has('sony interactive entertainment') || has('sony computer entertainment')) return 2;
    if (has('vendor: 045e') || has('vendor:045e') || l.indexOf('045e-') === 0 || has('xbox')) return 1;
    if (has('vendor: 057e') || has('vendor:057e') || l.indexOf('057e-') === 0 || has('nintendo') || has('joy-con')) return 3;
    return 0;
  }
  function padSnapshot(g) {
    var b = [], a = [], i;
    for (i = 0; i < g.buttons.length && i < 64; i++) b.push({ p: !!g.buttons[i].pressed, v: g.buttons[i].value });
    for (i = 0; i < g.axes.length && i < 64; i++) a.push(g.axes[i]);
    return { nb: g.buttons.length, na: g.axes.length, b: b, a: a };
  }
  // Against a stable baseline: slow movement adds up, static axes and small noise do not.
  function padChanged(g, base) {
    var i, v;
    for (i = 0; i < g.buttons.length && i < 64; i++) {
      if (!!g.buttons[i].pressed !== base.b[i].p) return true;
      v = g.buttons[i].value;
      if (isFinite(v)) {
        if (isFinite(base.b[i].v) && Math.abs(v - base.b[i].v) > 0.01) return true;
        if (!isFinite(base.b[i].v)) base.b[i].v = v;
      }
    }
    for (i = 0; i < g.axes.length && i < 64; i++) {
      v = g.axes[i];
      if (isFinite(v)) {
        if (isFinite(base.a[i]) && Math.abs(v - base.a[i]) > 0.01) return true;
        if (!isFinite(base.a[i])) base.a[i] = v;
      }
    }
    return false;
  }

  // Fire and forget: the host needs an id, but nothing is pending and no reply comes.
  // Without a connection the message is dropped (a stale pad state must not be queued).
  function padSend(cmd, f) {
    if (!remotePort) return false;
    var m = [{ key: 'cmd', value: cmd }, { key: 'id', value: String(nextId++) }];
    for (var k in f) m.push({ key: k, value: String(f[k]) });
    try { remotePort.sendMessage(m, localPort); return true; }
    catch (e) {   // the port is shared: only the normal command path may drop it
      if (!padTraceFail) { padTraceFail = true; trace('pad send failed', e && (e.name || e.message || e)); }
      return false;
    }
  }
  function padAxis(g, i) { var v = g.axes[i]; return isFinite(v) ? v : 0; }
  function padTrigger(g, i) { var b = g.buttons[i]; return b && isFinite(b.value) ? b.value : 0; }

  function pollPads() {
    var gps = (typeof navigator !== 'undefined' && navigator.getGamepads) ? navigator.getGamepads() : null;
    if (!gps) return;
    var mask = 0, removed = [], arrived = [], i;
    for (i = 0; i < PAD_MAX; i++) {
      var st = padSlots[i] || (padSlots[i] = { validated: false, announced: false, baseline: null, id: '', last: null, combo: false, since: 0 });
      var g = gps[i], connected = !!(g && g.connected), id = connected ? String(g.id) : '';
      var ts = connected ? g.timestamp : 0, same = st.id === id;
      var layout = connected && st.baseline && st.baseline.nb === g.buttons.length && st.baseline.na === g.axes.length;
      var real = connected && ((isFinite(ts) && ts > 0) || (same && (st.validated || (layout && padChanged(g, st.baseline)))));
      if (st.announced && (!real || !same)) { removed.push(i); st.announced = false; }
      if (!connected) { padSlots[i] = { validated: false, announced: false, baseline: null, id: '', last: null, combo: false, since: 0 }; continue; }
      if (!same) { st = padSlots[i] = { validated: false, announced: false, baseline: null, id: '', last: null, combo: false, since: 0 }; }
      st.id = id;
      if (!real) { if (!st.baseline || !layout) st.baseline = padSnapshot(g); continue; }
      st.validated = true;
      mask |= 1 << i;
      if (!st.announced) arrived.push(i);
    }
    removed.forEach(function (n) {
      padSlots[n].last = null;
      padSend('gamepadState', { index: n, mask: mask & ~(1 << n), buttons: 0, lt: 0, rt: 0, lsx: 0, lsy: 0, rsx: 0, rsy: 0 });
    });
    arrived.forEach(function (n) {   // a failed send leaves it un-announced: the next poll retries
      var g = gps[n];
      if (padSend('gamepadArrival', { index: n, mask: mask, type: padType(g.id), buttons: padSupported(g.buttons.length),
                                      caps: g.buttons.length > 7 ? 1 : 0 })) padSlots[n].announced = true;
    });
    for (i = 0; i < PAD_MAX; i++) {
      if (!(mask & (1 << i)) || !padSlots[i].announced) continue;
      var p = gps[i], fl = padButtonFlags(p);
      if (fl === STOP_COMBO) { window.Module.stopStream().then(null, function () {}); return; }
      if (fl === STATS_COMBO) { if (!padSlots[i].combo) { padSlots[i].combo = true; window.Module.toggleStats(); } }
      else padSlots[i].combo = false;
      var t = { index: i, mask: mask, buttons: fl, lt: Math.round(padTrigger(p, 6) * 255), rt: Math.round(padTrigger(p, 7) * 255),
                lsx: Math.round(padAxis(p, 0) * 32767), lsy: Math.round(-padAxis(p, 1) * 32767),
                rsx: Math.round(padAxis(p, 2) * 32767), rsy: Math.round(-padAxis(p, 3) * 32767) };
      var l = padSlots[i].last, drift = function (a, b) { return Math.abs(a - b) > PAD_STICK_EPS; };
      // send on a real change, a stick move > ~1 %, or every PAD_KEEPALIVE polls (recovers a dropped state, ends a drift exactly)
      if (!l || ++padSlots[i].since >= PAD_KEEPALIVE || l.mask !== t.mask || l.buttons !== t.buttons || l.lt !== t.lt || l.rt !== t.rt ||
          drift(l.lsx, t.lsx) || drift(l.lsy, t.lsy) || drift(l.rsx, t.rsx) || drift(l.rsy, t.rsy)) {
        if (padSend('gamepadState', t)) { padSlots[i].last = t; padSlots[i].since = 0; }
      }
    }
  }
  // Registered first (mlbridge loads before index.js): the hidden menu must not react to the pad during a stream.
  if (window.addEventListener) window.addEventListener('gamepadinputchanged', function (e) { if (padTimer) e.stopImmediatePropagation(); }, true);
  function startPads() {
    if (padTimer) return;
    padSlots = []; padTraceFail = false;
    padTimer = setInterval(function () { try { pollPads(); } catch (e) { console.warn('[mlbridge] gamepad poll failed:', e); } }, PAD_MS);
  }
  function stopPads() {
    if (padTimer) clearInterval(padTimer);
    padTimer = null; padSlots = [];   // the next stream announces the pads again
  }

  // The page goes away during a stream (the UI is closed): stop the stream, the host would keep it
  // running. Not visibilitychange: the host's video window may hide the page. stopPads() clears
  // padTimer, so the second event (pagehide, then unload) sends nothing.
  function onPageGone() {
    if (!padTimer) return;
    stopPads(); audioCall('mlAudioStop');
    bridgeCall('stopStream', {}, 0).then(null, function () {});
  }
  if (window.addEventListener) { window.addEventListener('pagehide', onPageGone); window.addEventListener('unload', onPageGone); }

  // Replies arrive as an array of {key,value}. We key everything by 'id'.
  var audioFailed = false;
  function onReply(data /*, remoteMessagePort */) {
    var m = {};
    for (var i = 0; i < data.length; i++) m[data[i].key] = data[i].value;
    if (m.total !== undefined) { m = join(m); if (!m) return; }
    if (m.event === 'mlaudio') {   // PCM for Web Audio (~200/s: not traced, not for handleMessage)
      if (typeof window.mlAudioFeed === 'function' && m.data !== undefined && typeof m.data !== 'string') {
        try { window.mlAudioFeed(m.rate, m.ch, m.samples, (m.data instanceof Uint8Array ? m.data : new Uint8Array(m.data)).buffer); }
        catch (e) { if (!audioFailed) { audioFailed = true; console.warn('[mlbridge] mlAudioFeed failed:', e); } }
      }
      return;
    }
    if (m.event === 'mlstats') { showStats(m); return; }   // overlay only (2/s: not traced), not for handleMessage
    if (m.event === 'mlaroute') {   // which audio path this session uses (not for handleMessage)
      if (m.route === 'native' || m.route === 'webaudio' || m.route === 'none') {
        audioRoute = m.route;
        if (audioRoute === 'native') audioCall('mlAudioStop');   // the engine plays audio itself: stop the Web Audio scheduler
        console.log('[mlbridge] audio route: ' + audioRoute);
      }
      return;
    }
    if (m.log !== undefined) {   // forwarded host/engine log line (on demand, see mlDebug); no reply trace: it doubled every line
      console.log('[' + (m.tag || 'host') + '] ' + m.log);
      return;
    }
    trace('reply id=' + m.id, 'keys=' + Object.keys(m).join(','), m.error ? 'error=' + m.error : '');
    if (m.event) {              // async push (streamTerminated/Connection Established)
      trace('event', m.event);
      if (m.diag) console.error('[ML-ERROR] ' + m.diag);   // always, not only when tracing
      if (String(m.event).indexOf('streamTerminated') === 0) { resetStats(); stopPads(); audioCall('mlAudioStop'); }
      else if (m.event === 'Connection Established') {
        startPads();
        if (wantStats && !statsOn) toggleStats().catch(function (e) { console.warn('toggleStats: ' + e); });
      }
      if (typeof window.handleMessage === 'function') window.handleMessage(String(m.event));
      return;
    }
    var p = pending[m.id];
    if (!p) { trace('reply for unknown/timed-out id=' + m.id + ' ignored'); return; }
    delete pending[m.id];
    if (p.timer) clearTimeout(p.timer);
    if (m.error) p.reject(m.error);
    else p.resolve(m);
  }

  // Send {cmd,id,...fields} and resolve with the reply map. timeoutMs 0 = no timeout.
  // cancelOnTimeout: also send cancelRequest when the timeout fires (openUrl).
  function bridgeCall(cmd, fields, timeoutMs, cancelOnTimeout) {
    return new Promise(function (resolve, reject) {
      var id = String(nextId++);
      var msg = [{ key: 'cmd', value: cmd }, { key: 'id', value: id }];
      var names = [];
      for (var k in fields) if (fields[k] !== undefined && fields[k] !== null) {
        msg.push({ key: k, value: String(fields[k]) });
        names.push(k);   // names only: cert/key/pin values must not be logged
      }
      pending[id] = { resolve: resolve, reject: reject, timer: null, msgs: split(msg), cmd: cmd,
                      timeoutMs: timeoutMs, cancelOnTimeout: cancelOnTimeout };
      trace('call cmd=' + cmd, 'id=' + id, 'fields=' + names.join(','));
      if (remotePort && !queue.length) sendNow(id);
      else { trace('queue id=' + id); queue.push(id); connect(); }
    });
  }

  function call(cmd, fields) { return bridgeCall(cmd, fields, DEFAULT_TIMEOUT); }

  // --- override the command maps used by messages.js sendMessage() ---
  // SyncFunctions returned {type,ret} synchronously in WASM; here they are async,
  // so we also override sendMessage below to await uniformly.
  window.SyncFunctions = {
    makeCert: function () { return call('makeCert', {}); },
    httpInit: function (cert, key, uniqueId) { return call('httpInit', { cert: cert, key: key, uniqueId: uniqueId }); },
    startRequest: function () {
      var a = arguments;
      audioCall('mlAudioStart');   // user-gesture flow: the autoplay policy needs it here
      padFlipAB = !!a[16]; padFlipXY = !!a[17];   // a[14] rumble and a[15] mouse emulation: not ported
      // host, httpPort, w, h, fps, bitrate, rikey, rikeyid, appVer, gfeVer, rtspUrl, then a[11] codecMode,
      // a[19] audioConfig (a[18] is audioBackend), a[24] videoCodec, a[25] hdrMode, a[26] fullRange,
      // a[27] gameMode (the TV's game picture/low-latency mode), a[29] performanceStats (overlay at stream start)
      wantStats = !!a[29];
      return call('startStream', {
        host: a[0], w: a[2], h: a[3], fps: a[4], bitrate: a[5], rikey: a[6], rikeyid: a[7],
        appVer: a[8], gfeVer: a[9], rtspUrl: a[10], codecMode: a[11],
        audioConfig: a[19], videoCodec: a[24], hdr: String(!!a[25]), fullRange: String(!!a[26]),
        gameMode: String(!!a[27])
      });
    },
    stopRequest: function () { stopPads(); audioCall('mlAudioStop'); return call('stopStream', {}); },
    cancelRequest: function () { return call('cancelRequest', {}); },
    toggleStats: toggleStats,
  };
  window.AsyncFunctions = {
    openUrl: function (url, ppk, binary, timeout) {
      return bridgeCall('openUrl', { url: url, ppk: ppk, binary: String(!!binary) },
                        timeout > 0 ? timeout : DEFAULT_TIMEOUT, true);
    },
    STUN: function () { return call('stun', {}); },
    pair: function (major, address, httpPort, pin) {
      return bridgeCall('pair', { major: major, addr: address, port: httpPort, pin: pin }, PAIR_TIMEOUT);
    },
    wakeOnLan: function (mac) { return call('wakeOnLan', { mac: mac }); },
  };

  // Uniform async sendMessage over the bridge (replaces the WASM-era mix of
  // sync/async). Returns the reply's primary field for back-compat where callers
  // expect a value (openUrl -> data, pair -> serverCert, makeCert -> {cert,privateKey}).
  window.sendMessage = function (method, params) {
    params = params || [];
    var fn = window.SyncFunctions[method] || window.AsyncFunctions[method];
    trace('sendMessage', method);
    if (!fn) trace('unknown method', method);
    if (!fn) return Promise.reject('unknown method ' + method);
    return fn.apply(null, params).then(function (r) {
      if (method === 'makeCert') return { cert: r.cert, privateKey: r.key };
      if (method === 'openUrl') {
        var d = r.data;
        if (params[2] && typeof d !== 'string' && d != null) return new Uint8Array(d);   // bytes -> Uint8Array
        return d;
      }
      if (method === 'pair') return r.serverCert;
      return r.result;
    });
  };


  // Some UI code calls Module.<fn> directly (not via sendMessage), notably
  // Module.sendKeyboardEvent during streaming. Provide a shim so the UI needs no
  // WASM module. HEAPF* (Emscripten perf-stats heap access) is stubbed - the
  // in-stream stats overlay that reads WASM memory is not available here.
  window.Module = {
    sendKeyboardEvent: function (keyCode, keyAction, modifiers) {
      return bridgeCall('sendKeyboardEvent', { key: keyCode, action: keyAction, mods: modifiers });
    },
    stopStream: function () { stopPads(); audioCall('mlAudioStop'); return bridgeCall('stopStream', {}); },
    // the yellow-key caller ignores the promise: log a failure instead of an unhandled rejection
    toggleStats: function () { return toggleStats().catch(function (e) { console.warn('toggleStats: ' + e); }); },
    HEAPF32: { buffer: new ArrayBuffer(0) },   // stub; perf-stats overlay disabled
    HEAPF64: { buffer: new ArrayBuffer(0) },
  };

  // Update check: moonlight-tizen's index.js asks GitHub for ITS latest release and
  // offers that package as an update. Those packages do not fit this client, so the
  // check is off. The functions stay in index.js; to point them at this project's
  // releases later, set __REPO_OWNER__/__REPO_NAME__ in pkg/build-package.sh (index.js
  // reads them in fetchLatestRelease), keep config.xml's version in step with the
  // release tags (checkVersionUpdate compares against it) and set UPDATE_CHECK = true.
  var UPDATE_CHECK = false;
  function disableUpdateCheck() {
    if (UPDATE_CHECK) return;
    ['checkForAppUpdates', 'checkForAppUpdatesAtStartup', 'updateAppButton', 'updateAppDialog'].forEach(function (n) {
      window[n] = function () { trace('update check disabled:', n); };
    });
    window.fetchLatestRelease = function () { return Promise.reject(new Error('update check disabled')); };
  }

  // Settings > About shows "Moonlight v<version> (<build>)": which fork, variant,
  // commit and base UI version (build-package.sh fills ML_BUILD). index.js would show
  // only the version, or the base UI's commit in debug.
  var ML_BUILD = '__ML_BUILD__';
  function showBuild() {
    window.getBuildVersion = function (version) {
      return version + ' (' + (ML_BUILD.indexOf('__') === 0 ? 'tizen50-core dev' : ML_BUILD) + ')';
    };
  }

  // On-demand host/engine log in this console (for example Apps2Samsung's debug
  // console): mlDebug(true) / mlDebug(false). Remembered across restarts. The dlog
  // tags (MLHOST, MLENGINE, MLUI) are written as before either way.
  var DEBUG_KEY = 'mlDebug';
  // On by default in the debug packages (mltrace.js sets ML_TRACE); an explicit
  // mlDebug(false) is remembered and wins.
  function debugStored() {
    var v = null;
    try { v = localStorage.getItem(DEBUG_KEY); } catch (e) {}
    return v === '1' || (v === null && TRACE);
  }
  window.mlDebug = function (on) {
    on = !!on;
    try { localStorage.setItem(DEBUG_KEY, on ? '1' : '0'); } catch (e) {}
    call('setLog', { on: String(on) }).then(null, function (e) { console.warn('[mlbridge] setLog failed:', e); });
    return 'host/engine log in this console: ' + (on ? 'on' : 'off');
  };

  // mlGameMode(false) switches the TV game mode off now (it can stay on after the app was
  // closed); mlGameMode(true) on. saveType is optional (the host default is 1, the session's).
  window.mlGameMode = function (on, saveType) {
    bridgeCall('tvGameMode', { on: String(!!on), saveType: saveType }, DEFAULT_TIMEOUT).then(function (r) {
      console.log('[mlbridge] TV game mode ' + (on ? 'on' : 'off') + ' -> rc ' + r.rc);
    }, function (e) {
      console.warn('[mlbridge] TV game mode failed:', e);
    });
    return 'TV game mode ' + (on ? 'on' : 'off') + ' requested';
  };

  // mlStatsOsd(true|false): the engine lowers (true) or raises (false) its video window, so the stats
  // overlay shows over the picture. rc: 0 ok, -1 not supported by this TV. window: 1 lowered, 2 raised,
  // 0 no stream window yet (applied when the next stream starts). The state is kept for later streams.
  window.mlStatsOsd = function (on) {
    bridgeCall('statsOsd', { on: String(!!on) }, DEFAULT_TIMEOUT).then(function (r) {
      var msg = '[mlbridge] stats over video ' + (on ? 'on' : 'off') + ' -> rc ' + r.rc + ' window ' + r.win;
      if (String(r.rc) === '0') console.log(msg); else console.warn(msg);
    }, function (e) {
      console.warn('[mlbridge] stats over video failed:', e);
    });
    return 'stats over video ' + (on ? 'on' : 'off') + ' requested';
  };

  // mlTargetDepth(ms): the latency target of the nudge pacing, 20..500 ms (clamped by the engine), kept for
  // the life of the app; it also turns on nudge pacing. rc is the applied value, -1 = refused (negative).
  window.mlTargetDepth = function (ms) {
    bridgeCall('targetDepth', { ms: String(ms) }, DEFAULT_TIMEOUT).then(function (r) {
      var msg = '[mlbridge] target depth ' + ms + ' -> rc ' + r.rc + (r.ms !== undefined ? ' ms ' + r.ms : '');
      if (String(r.rc) !== '-1') console.log(msg); else console.warn(msg);
    }, function (e) {
      console.warn('[mlbridge] target depth failed:', e);
    });
    return 'target depth ' + ms + ' requested';
  };

  // Diagnostics in this console (on a retail TV, Apps2Samsung's debug console is the
  // only place to read logs):
  //   mlDiag()          the host runs its engine load checks again and sends the report
  //   mlDiag('launch')  start the host in each way the UI can, and say what happened
  var DIAG_TIMEOUT = 60000;
  window.mlDiag = function (what) {
    if (what === 'launch') { diagLaunch(); return 'starting the host in each way; see the [mldiag] lines'; }
    console.log('[mldiag] asking the host to run its checks');
    bridgeCall('diag', { rerun: 'true' }, DIAG_TIMEOUT).then(function (r) {
      String(r.report || '').split('\n').forEach(function (l) { console.log('[MLDIAG] ' + l); });
      console.log('[mldiag] report end');
    }, function (e) {
      console.warn('[mldiag] no report from the host: ' + e + '. Is it running? Try mlDiag("launch").');
    });
    return 'host diagnostics requested';
  };

  // Each way the UI can start the host, one after another while it is not running.
  // The UI cannot stop the host, so once one way starts it the later ways are skipped.
  function diagLaunch() {
    var ways = [
      ['tizen.application.launch', function (ok, err) { tizen.application.launch(ENGINE_APPID, ok, err); }],
      ['tizen.application.launchAppControl (operation default, explicit app id)', function (ok, err) {
        var ac = new tizen.ApplicationControl('http://tizen.org/appcontrol/operation/default', null, null, null, null);
        tizen.application.launchAppControl(ac, ENGINE_APPID, ok, err, null);
      }]
    ];
    try { var me = tizen.application.getCurrentApplication().appInfo; console.log('[mldiag] UI ' + me.id + ' package ' + me.packageId + ' version ' + me.version); }
    catch (e) { console.log('[mldiag] UI app info unavailable: ' + errText(e)); }
    hostReported = false; reportHost();   // install state again, also when said before
    hostState('before', function (up) {
      if (up) { console.log('[mldiag] the host already runs: close it (or restart the TV) to test how it starts'); return; }
      next(0);
    });
    function next(i) {
      if (i >= ways.length) { console.log('[mldiag] launch test end: no way started the host'); return; }
      var name = ways[i][0];
      console.log('[mldiag] way ' + (i + 1) + ': ' + name);
      function check() {
        setTimeout(function () {
          hostState('3 s after way ' + (i + 1), function (up) {
            if (up) console.log('[mldiag] launch test end: way ' + (i + 1) + ' (' + name + ') started the host');
            else next(i + 1);
          });
        }, 3000);
      }
      try {
        ways[i][1](function () { console.log('[mldiag] way ' + (i + 1) + ' accepted'); check(); },
                   function (e) { console.log('[mldiag] way ' + (i + 1) + ' error callback: ' + errText(e)); check(); });
      } catch (e) { console.log('[mldiag] way ' + (i + 1) + ' threw: ' + errText(e)); next(i + 1); }
    }
  }

  // Issue #21: the UI keeps its hosts in IndexedDB, and a start from Apps2Samsung (debug)
  // and a start from the remote can use separate WebView storage. wgt-private is per
  // package, not per WebView profile, so the key/value store is mirrored in a file there.
  // Any failure logs one line and the UI goes on with IndexedDB only.
  var STORE_FILE = 'mlstore.json', mirror = {}, storeDir = null, fileState = 'unavailable';
  var writing = false, dirty = false, diagDone = false;
  function storeWarn(what, e) { console.warn('[mlbridge] storage: ' + what + (e ? ': ' + errText(e) : '')); }
  function isObj(v) { return !!v && typeof v === 'object'; }
  function flushStore() {
    if (writing) { dirty = true; return; }
    writing = true; dirty = false;
    function end(what, e) { if (what) storeWarn(what, e); writing = false; if (dirty && !what) flushStore(); }
    try {
      var tmp = STORE_FILE + '.tmp', str = JSON.stringify(mirror);
      try { storeDir.resolve(tmp); } catch (e) { storeDir.createFile(tmp); }
      storeDir.resolve(tmp).openStream('w', function (st) {
        try {
          st.write(str); st.close();
          storeDir.moveTo(storeDir.fullPath + '/' + tmp, storeDir.fullPath + '/' + STORE_FILE, true,
                          function () { end(); }, function (e) { end('rename failed', e); });
        } catch (e) { end('write failed', e); }
      }, function (e) { end('open failed', e); }, 'UTF-8');
    } catch (e) { end('write failed', e); }
  }
  function loadStore(done) {
    var over = false;
    function finish(wrap) { if (over) return; over = true; if (wrap) installStore(); done(); }
    setTimeout(function () { if (!over) storeWarn('read timeout'); finish(false); }, 2000);
    try {
      tizen.filesystem.resolve('wgt-private', function (dir) {
        storeDir = dir;
        try {
          var f; try { f = dir.resolve(STORE_FILE); } catch (e) { fileState = 'missing'; finish(true); return; }
          f.readAsText(function (txt) {
            try { var o = JSON.parse(txt); if (!isObj(o)) throw 0; mirror = o; fileState = 'ok'; }
            catch (e) { fileState = 'bad json'; storeWarn('bad json in file'); }
            finish(true);
          }, function (e) { storeWarn('read failed', e); finish(false); }, 'UTF-8');
        } catch (e) { storeWarn('read failed', e); finish(false); }
      }, function (e) { storeWarn('no private dir', e); finish(false); }, 'rw');
    } catch (e) { storeWarn('no private dir', e); finish(false); }
  }
  function installStore() {
    var origGet = window.getData, origStore = window.storeData;
    function clone(v) { var j = JSON.stringify(v); return j === undefined ? null : JSON.parse(j); }
    window.storeData = function (key, data, cb) {
      origStore.call(window, key, data, cb);
      mirror[key] = clone(data); flushStore();
    };
    window.getData = function (key, cb) {
      origGet.call(window, key, function (obj) {
        try {
          var idb = obj[key], idbJson = JSON.stringify(idb), used = 'none', merged = 0, inFile = 0, inIdb = 0;
          if (key === 'hosts') {
            inFile = isObj(mirror.hosts) ? Object.keys(mirror.hosts).length : 0;
            inIdb = isObj(idb) ? Object.keys(idb).length : 0;
            var done = false;
            try { done = localStorage.getItem('mlstoreHostsMerged') === '1'; } catch (e) {}
            if (!done) {
              if (isObj(mirror.hosts) && isObj(idb)) {
                for (var uid in idb) if (!(uid in mirror.hosts)) { mirror.hosts[uid] = idb[uid]; merged++; }
                if (merged) flushStore();
              }
              try { localStorage.setItem('mlstoreHostsMerged', '1'); } catch (e) {}
            }
          }
          if (mirror[key] != null) {
            obj[key] = clone(mirror[key]); used = 'file';
            if (JSON.stringify(mirror[key]) !== idbJson) origStore.call(window, key, mirror[key], function () {});
          } else if (idb != null) {
            mirror[key] = clone(idb); used = 'indexeddb'; flushStore();
          }
          if (key === 'hosts' && !diagDone) {
            diagDone = true;
            var me = {}; try { me = tizen.application.getCurrentApplication().appInfo; } catch (e) {}
            console.log('[mlbridge] storage: app ' + (me.id || '?') + ' package ' + (me.packageId || '?') +
                        ', hosts in IndexedDB ' + inIdb + ', in file ' + inFile + ', used ' + used +
                        ', file ' + fileState + ', merged ' + merged);
          }
        } catch (e) { storeWarn('getData failed', e); }
        cb(obj);
      });
    };
  }

  // Boot without the WASM engine. The original starts the UI from the compiled
  // module: common.js on DOMContentLoaded calls createWASMModule() -> loads
  // moonlight-wasm.js -> (on WASM ready) window.moduleDidLoad(), which starts the
  // app. We ship NO moonlight-wasm.js (the engine is the native service), so that
  // chain would never fire moduleDidLoad and the UI would hang. mlbridge.js is parsed
  // before common.js, so this DOMContentLoaded listener runs first and replaces
  // common.domContentLoaded to skip the WASM load and fire moduleDidLoad() ourselves.
  document.addEventListener('DOMContentLoaded', function () {
    disableUpdateCheck();   // index.js is loaded now; runs before attachListeners binds the button
    showBuild();
    trace('DOMContentLoaded: common.domContentLoaded exists =',
          !!(window.common && typeof window.common.domContentLoaded === 'function'));
    if (window.common && typeof window.common.domContentLoaded === 'function') {
      window.common.domContentLoaded = function () {
        try { if (window.common.attachDefaultListeners) window.common.attachDefaultListeners(); } catch (e) {}
        if (TRACE) {
          try { var me = tizen.application.getCurrentApplication().appInfo; trace('ui app ' + me.id + ', package ' + me.packageId + ', version ' + me.version); }
          catch (e) { trace('ui app info unavailable: ' + errText(e)); }
          try { trace('platform ' + tizen.systeminfo.getCapability('http://tizen.org/feature/platform.version')); } catch (e) {}
          console.log('[mlbridge] diagnostics: mlDiag() = host engine checks, mlDiag("launch") = host start test');
        }
        connect();   // start/connect the host (commands queue until connected)
        if (debugStored()) window.mlDebug(true);
        trace('boot: moduleDidLoad found =', typeof window.moduleDidLoad === 'function');
        function start() { if (typeof window.moduleDidLoad === 'function') window.moduleDidLoad(); }
        if (typeof tizen !== 'undefined' && tizen.filesystem && typeof window.getData === 'function' && typeof window.storeData === 'function') loadStore(start);
        else start();
      };
    }
  }, false);

  console.log('[mlbridge] installed (message-port transport to ' + ENGINE_APPID + ')');
})();
