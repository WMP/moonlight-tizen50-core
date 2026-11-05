// Host self-test for pkg/ui/mlbridge.js: node tests/mlbridge_test.js
var fs = require('fs'), vm = require('vm'), assert = require('assert');

var logs = [], warns = [], errs = [], timers = [], now = 0, tid = 1;
var byId = {}, domReady = null, remoteUp = false, hostDead = false, launchFails = false, launches = 0, sentAt = 0, sent = [], listener = null, handled = [];
var cleared = {}, pads = [];
var padListener = null, padCapture = null;
var keyListener = null, keyCapture = null;
var win = { addEventListener: function (ev, f, cap) { if (ev === 'gamepadinputchanged') { padListener = f; padCapture = cap; } if (ev === 'keydown') { keyListener = f; keyCapture = cap; } }, ML_TRACE: true, handleMessage: function (m) { handled.push(m); } };
var tizen = {
  ApplicationControl: function (op) { this.operation = op; },
  ApplicationControlData: function (k, v) { this.key = k; this.value = v; },
  application: {
    launchAppControl: function () { throw new Error('the UI must never start the host'); },
    launch: function (id, ok) {
      assert.strictEqual(id, 'org.moonlight.host.app');
      launches++;
      if (!launchFails) remoteUp = true;   // the host starts and opens its port
      ok();
    },
    getAppInfo: function (id) { return { id: id, version: '9.9.9', packageId: 'org.moonlight.host' }; },
    getAppsContext: function (ok) { ok([{ appId: 'Moonlight5.ui' }]); }
  },
  messageport: {
    requestLocalMessagePort: function () { return { addMessagePortListener: function (f) { listener = f; } }; },
    requestRemoteMessagePort: function () {
      if (!remoteUp) { var e = new Error('x'); e.name = 'NotFoundError'; throw e; }
      return { sendMessage: function (m) { if (hostDead) throw new Error('gone'); sent.push(m); sentAt = now; } };
    }
  }
};
var sandbox = {
  window: win, tizen: tizen, Promise: Promise, Uint8Array: Uint8Array, ArrayBuffer: ArrayBuffer,
  localStorage: { _d: {}, getItem: function (k) { return this._d[k] === undefined ? null : this._d[k]; }, setItem: function (k, v) { this._d[k] = String(v); } },
  document: {
    addEventListener: function (ev, f) { if (ev === 'DOMContentLoaded') domReady = f; },
    body: { appendChild: function (e) { byId[e.id] = e; } },
    createElement: function () { return { style: {}, textContent: '' }; },
    getElementById: function (id) { return byId[id] || null; }
  },
  console: { log: function () { logs.push([].join.call(arguments, ' ')); }, warn: function () { warns.push([].join.call(arguments, ' ')); }, error: function () { errs.push([].join.call(arguments, ' ')); } },
  setTimeout: function (f, ms) { timers.push({ id: tid, at: now + ms, f: f }); return tid++; },
  clearTimeout: function (id) { cleared[id] = true; timers = timers.filter(function (t) { return t.id !== id; }); },
  setInterval: function (f, ms) { timers.push({ id: tid, at: now + ms, f: f, every: ms }); return tid++; },
  navigator: { getGamepads: function () { return pads; } }
};
sandbox.clearInterval = sandbox.clearTimeout;
function advance(ms) {
  var end = now + ms;
  for (;;) {
    timers.sort(function (a, b) { return a.at - b.at; });
    if (!timers.length || timers[0].at > end) break;
    var t = timers.shift(); now = t.at; t.f();
    if (t.every && !cleared[t.id]) { t.at = now + t.every; timers.push(t); }
  }
  now = end;
}
function tick() { return new Promise(function (r) { setImmediate(r); }); }
function fields(m) { var o = {}; m.forEach(function (kv) { o[kv.key] = kv.value; }); return o; }
function reply(id, o) {
  var d = [{ key: 'id', value: id }];
  for (var k in o) d.push({ key: k, value: o[k] });
  listener(d);
}

vm.runInNewContext(fs.readFileSync(__dirname + '/../pkg/ui/mlbridge.js', 'utf8'), sandbox);
sandbox.window = win;
var tests = [];
function test(n, f) { tests.push([n, f]); }

test('queue while host is down, send after it comes up', async function () {
  var p = win.sendMessage('makeCert', []);
  assert.strictEqual(sent.length, 0);
  assert.strictEqual(launches, 1);    // the first miss starts the host
  advance(250);                       // first retry connects and sends
  assert.deepStrictEqual(warns, ['[mlbridge] host org.moonlight.host.app installed (version 9.9.9, package org.moonlight.host), not running']);
  assert.strictEqual(sent.length, 1);
  assert.strictEqual(launches, 1);
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'makeCert');
  reply(f.id, { cert: 'CERTPEM', key: 'KEYPEM' });
  var c = await p;
  assert.strictEqual(c.cert, 'CERTPEM'); assert.strictEqual(c.privateKey, 'KEYPEM');
});

test('openUrl mapping and binary reply', async function () {
  sent.length = 0;
  var p = win.sendMessage('openUrl', ['https://h/serverinfo', 'PPKPEM', true, 5000]);
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'openUrl'); assert.strictEqual(f.url, 'https://h/serverinfo');
  assert.strictEqual(f.ppk, 'PPKPEM'); assert.strictEqual(f.binary, 'true');
  reply(f.id, { data: [1, 2, 255] });
  var r = await p;
  assert.ok(r instanceof Uint8Array); assert.strictEqual(Array.prototype.join.call(r, ','), '1,2,255');
  var p2 = win.sendMessage('openUrl', ['u', 'k', false, 0]);
  reply(fields(sent[1]).id, { data: '<xml/>' });
  assert.strictEqual(await p2, '<xml/>');
});

test('startRequest mapping', async function () {
  sent.length = 0;
  var a = []; a[0] = 'host'; a[1] = 47989; a[2] = 1920; a[3] = 1080; a[4] = 60; a[5] = 20000; a[6] = 'abcd';
  a[7] = 7; a[8] = '7.1'; a[9] = '3.2'; a[10] = 'rtsp://x'; a[11] = 1; a[19] = '51Surround'; a[24] = 'HEVC'; a[25] = true;
  var p = win.sendMessage('startRequest', a);
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'startStream');
  var want = { host: 'host', w: '1920', h: '1080', fps: '60', bitrate: '20000', rikey: 'abcd', rikeyid: '7',
    appVer: '7.1', gfeVer: '3.2', rtspUrl: 'rtsp://x', codecMode: '1', audioConfig: '51Surround', videoCodec: 'HEVC', hdr: 'true' };
  for (var k in want) assert.strictEqual(f[k], want[k], k);
  reply(f.id, {});
  assert.strictEqual(await p, undefined);
});

test('startRequest full 30-argument order', async function () {
  sent.length = 0;
  var a = ['host', 47989, 1920, 1080, 60, 20000, 'abcd', 7, '7.1', '3.2', 'rtsp://x', 3, 1, 0, 1, 0, 0, 0,
    'EMSS', '51Surround', 1, 40, 1, 1, 'HEVC', 1, 1, 0, 0, 1];
  assert.strictEqual(a.length, 30);
  var p = win.sendMessage('startRequest', a);
  var f = fields(sent[0]);
  var want = { audioConfig: '51Surround', fullRange: 'true', videoCodec: 'HEVC', hdr: 'true', codecMode: '3' };
  for (var k in want) assert.strictEqual(f[k], want[k], k);
  reply(f.id, {});
  await p;
});

test('startRequest fullRange defaults to false', async function () {
  sent.length = 0;
  var p = win.sendMessage('startRequest', ['host', 47989, 1920, 1080, 60, 20000, 'abcd', 7]);
  var f = fields(sent[0]);
  assert.strictEqual(f.fullRange, 'false');
  reply(f.id, {});
  await p;
  sent.length = 0;
  var a = []; a[26] = 0;
  p = win.sendMessage('startRequest', a);
  f = fields(sent[0]);
  assert.strictEqual(f.fullRange, 'false');
  reply(f.id, {});
  await p;
});

test('pair mapping and serverCert', async function () {
  sent.length = 0;
  var p = win.sendMessage('pair', [7, '1.2.3.4', 47989, '1234', 'uid']);
  var f = fields(sent[0]);
  assert.strictEqual(f.major, '7'); assert.strictEqual(f.addr, '1.2.3.4');
  assert.strictEqual(f.port, '47989'); assert.strictEqual(f.pin, '1234');
  reply(f.id, { serverCert: 'SC' });
  assert.strictEqual(await p, 'SC');
});

test('timeouts and late reply', async function () {
  sent.length = 0;
  var p = win.sendMessage('openUrl', ['u', 'k', false, 3000]);
  var id = fields(sent[0]).id;
  advance(3000);
  await assert.rejects(p, function (e) { return e === 'timeout'; });
  assert.strictEqual(sent.length, 2);
  assert.strictEqual(fields(sent[1]).cmd, 'cancelRequest');
  reply(id, { data: 'late' });        // ignored, must not throw
  var q = win.sendMessage('httpInit', ['c', 'k', 'u']);
  advance(15000);
  await assert.rejects(q, function (e) { return e === 'timeout'; });
  assert.strictEqual(sent.length, 3);   // no cancelRequest for non-openUrl
});

test('timeout starts at send, queue re-sends on reconnect, pending reply resolves', async function () {
  sent.length = 0;
  hostDead = true; remoteUp = false; launchFails = true;   // host died: next send fails, port is gone
  var done = null;
  var p = win.sendMessage('httpInit', ['c', 'k', 'u']);
  p.then(function () { done = 'ok'; }, function (e) { done = 'err ' + e; });
  assert.strictEqual(sent.length, 0);
  advance(60000);                              // much longer than 15 s, but never sent
  await tick();
  assert.strictEqual(done, null, 'must not time out while queued');
  hostDead = false; remoteUp = true; launchFails = false;  // host is back
  advance(4000);                               // next retry reconnects and flushes
  assert.strictEqual(sent.length, 1);
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'httpInit');
  advance(sentAt + 14000 - now);               // 14 s after the send: still pending
  await tick();
  assert.strictEqual(done, null);
  reply(f.id, {});
  await tick();
  assert.strictEqual(done, 'ok', 'late but still pending reply resolves');
  var q = win.sendMessage('httpInit', ['c', 'k', 'u']);
  q.then(null, function () {});
  assert.strictEqual(sentAt, now);             // connected: sent at once
  advance(15000);                              // armed at send: 15 s after it -> timeout
  await assert.rejects(q, function (e) { return e === 'timeout'; });
});

test('a failing launch is repeated on the 9th miss, not before', async function () {
  sent.length = 0;
  hostDead = true; remoteUp = false; launchFails = true;
  var l0 = launches;
  var p = win.sendMessage('httpInit', ['c', 'k', 'u']);
  p.then(null, function () {});
  advance(250);                                // miss 1
  assert.strictEqual(launches, l0 + 1);
  advance(19500);                              // misses 2..8 (backoff 250, 500, ... 4 s)
  assert.strictEqual(launches, l0 + 1);
  advance(4000);                               // miss 9
  assert.strictEqual(launches, l0 + 2);
  hostDead = false; remoteUp = true; launchFails = false;
  advance(4000);                               // connects, sends
  reply(fields(sent[0]).id, {});
  await p;
});

test('upstream update check is disabled', async function () {
  var called = [];
  ['checkForAppUpdates', 'checkForAppUpdatesAtStartup', 'updateAppButton', 'updateAppDialog', 'fetchLatestRelease']
    .forEach(function (n) { win[n] = function () { called.push(n); }; });
  domReady();
  win.checkForAppUpdates(); win.checkForAppUpdatesAtStartup(); win.updateAppButton('9'); win.updateAppDialog('9', '');
  await assert.rejects(win.fetchLatestRelease());
  assert.strictEqual(called.length, 0, 'original update functions must not run: ' + called);
});

test('multi-part replies join; large commands are split', async function () {
  sent.length = 0;
  var xml = '<root>' + new Array(3000).join('x') + '</root>';
  var p = win.sendMessage('openUrl', ['https://h/applist', null, false, 0]);
  var id = fields(sent[0]).id;
  var parts = [xml.slice(0, 1000), xml.slice(1000, 2000), xml.slice(2000)];
  [2, 0, 1].forEach(function (seq) {          // out of order, plus one duplicate
    listener([{ key: 'id', value: id }, { key: 'seq', value: String(seq) }, { key: 'total', value: '3' },
              { key: 'field', value: 'data' }, { key: 'chunk', value: parts[seq] }]);
  });
  assert.strictEqual(await p, xml);
  var pb = win.sendMessage('openUrl', ['https://h/appasset', null, true, 0]);
  var idb = fields(sent[1]).id;
  listener([{ key: 'id', value: idb }, { key: 'seq', value: '0' }, { key: 'total', value: '2' }, { key: 'field', value: 'data' }, { key: 'chunk', value: [1, 2] }]);
  listener([{ key: 'id', value: idb }, { key: 'seq', value: '1' }, { key: 'total', value: '2' }, { key: 'field', value: 'data' }, { key: 'chunk', value: [3] }]);
  var r = await pb;
  assert.ok(r instanceof Uint8Array); assert.strictEqual(Array.prototype.join.call(r, ','), '1,2,3');

  sent.length = 0;                             // UI -> host: a ~3 KB httpInit is split
  var cert = new Array(1400).join('C'), key = new Array(1700).join('K');
  var q = win.sendMessage('httpInit', [cert, key, 'uid']);
  assert.ok(sent.length > 1, 'split into parts');
  var got = {};
  sent.forEach(function (m) {
    var f = fields(m);
    assert.strictEqual(f.cmd, 'httpInit'); assert.strictEqual(f.total, String(sent.length));
    var bytes = m.reduce(function (n, kv) { return n + kv.key.length + String(kv.value).length; }, 0);
    assert.ok(bytes <= 2048, 'part size ' + bytes);
    got[f.field] = (got[f.field] || '') + f.chunk;
  });
  assert.strictEqual(got.cert, cert); assert.strictEqual(got.key, key); assert.strictEqual(got.uniqueId, 'uid');
  reply(fields(sent[0]).id, {});
  await q;
});

test('mlDebug toggles host log forward; log lines go to the console', async function () {
  sent.length = 0;
  var r = win.mlDebug(true);
  assert.ok(/on/.test(r));
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'setLog'); assert.strictEqual(f.on, 'true');
  assert.strictEqual(sandbox.localStorage.getItem('mlDebug'), '1');
  reply(f.id, {});
  var before = handled.length;
  var nlog = logs.length;
  listener([{ key: 'log', value: 'engine loaded from memory' }, { key: 'tag', value: 'MLHOST' }]);
  assert.ok(logs.indexOf('[MLHOST] engine loaded from memory') >= 0, 'printed with tag');
  assert.strictEqual(logs.length - nlog, 1, 'one console line per forwarded line (no reply trace)');
  assert.strictEqual(handled.length, before, 'not an event');
  win.mlDebug(false);
  assert.strictEqual(fields(sent[1]).on, 'false');
  assert.strictEqual(sandbox.localStorage.getItem('mlDebug'), '0');
});

test('private keys are masked in console output', async function () {
  var n = logs.length;
  sandbox.console.log('DB key: cert with value: {"privateKey":"-----BEGIN PRIVATE KEY-----\\nMIIEvQ\\nSECRETKEY\\n-----END PRIVATE KEY-----\\n"}');
  var line = logs.slice(n).join('\n');
  assert.ok(line.indexOf('SECRETKEY') < 0 && line.indexOf('[redacted]') >= 0, line);
});

test('debug package turns host log forwarding on at boot; mlDebug(false) is remembered', async function () {
  function boot() {
    win.common = { domContentLoaded: function () {}, attachDefaultListeners: function () {} };
    domReady();
    win.common.domContentLoaded();
  }
  sandbox.localStorage._d = {};              // fresh install of the debug package (ML_TRACE is on)
  sent.length = 0; boot();
  var setLog = sent.map(fields).filter(function (f) { return f.cmd === 'setLog'; });
  assert.strictEqual(setLog.length, 1); assert.strictEqual(setLog[0].on, 'true');
  sandbox.localStorage._d = { mlDebug: '0' }; // user turned it off before
  sent.length = 0; boot();
  assert.strictEqual(sent.map(fields).filter(function (f) { return f.cmd === 'setLog'; }).length, 0);
});

test('mlDiag prints the host report line by line', async function () {
  sent.length = 0;
  var r = win.mlDiag();
  assert.ok(/requested/.test(r));
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'diag'); assert.strictEqual(f.rerun, 'true');
  reply(f.id, { report: 'diag#1 run: x\ndiag#1 summary: file: read=ok exec=ok, dlopen ok' });
  await tick();
  assert.ok(logs.indexOf('[MLDIAG] diag#1 run: x') >= 0);
  assert.ok(logs.indexOf('[MLDIAG] diag#1 summary: file: read=ok exec=ok, dlopen ok') >= 0);
});

test('mlDiag("launch") tries each way while the host does not run', async function () {
  var n = logs.length, l0 = launches;
  win.mlDiag('launch');
  assert.strictEqual(launches, l0 + 1, 'way 1 = launch');
  advance(3000);                     // check after way 1: the stub never lists the host -> way 2
  var out = logs.slice(n).join('\n');
  assert.ok(/before: host process not running/.test(out), out);
  assert.ok(/way 1 accepted/.test(out), out);
  assert.ok(/way 2 threw: .*must never start the host/.test(out), out);
  assert.ok(/launch test end: no way started the host/.test(out), out);
});

test('event passes string', async function () {
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  assert.strictEqual(handled.join('|'), 'streamTerminated: 0');
});

test('event with diag logs the ML-ERROR line and passes the code', async function () {
  var line = 'ML-ERROR code=-101 name=NO_VIDEO_FRAME stage=- t=10412ms | cfg 1920x1080@60';
  var before = handled.length;
  listener([{ key: 'event', value: 'streamTerminated: -101' }, { key: 'diag', value: line }]);
  assert.strictEqual(handled.slice(before).join('|'), 'streamTerminated: -101');
  assert.ok(errs.indexOf('[ML-ERROR] ' + line) >= 0, errs.join('\n'));
});

test('event with a long diag sent in parts is joined', async function () {
  var line = 'ML-ERROR code=-101 ' + new Array(1500).join('x');
  var a = line.slice(0, 512), b = line.slice(512);
  var before = handled.length;
  listener([{ key: 'id', value: 'ev7' }, { key: 'seq', value: '1' }, { key: 'total', value: '3' }, { key: 'field', value: 'diag' }, { key: 'chunk', value: a }]);
  listener([{ key: 'id', value: 'ev7' }, { key: 'seq', value: '0' }, { key: 'total', value: '3' }, { key: 'field', value: 'event' }, { key: 'chunk', value: 'streamTerminated: -101' }]);
  assert.strictEqual(handled.length, before);   // not all parts yet
  listener([{ key: 'id', value: 'ev7' }, { key: 'seq', value: '2' }, { key: 'total', value: '3' }, { key: 'field', value: 'diag' }, { key: 'chunk', value: b }]);
  assert.strictEqual(handled.slice(before).join('|'), 'streamTerminated: -101');
  assert.ok(errs.indexOf('[ML-ERROR] ' + line) >= 0);
});

var STATS = [{ key: 'event', value: 'mlstats' }, { key: 'depth', value: '12' }, { key: 'addepth', value: '8' }, { key: 'fps', value: '60' },
  { key: 'drop', value: '0' }, { key: 'full', value: '0' }, { key: 'uo', value: '0/0' }, { key: 'err', value: '0' }, { key: 'rec', value: '0' }, { key: 'play', value: '12300' }];
async function toggleStats(err, on) {
  var n = sent.length, p = win.SyncFunctions.toggleStats();
  advance(250);
  assert.strictEqual(sent.length, n + 1);
  var f = fields(sent[n]);
  assert.strictEqual(f.cmd, 'toggleStats');
  if (on !== undefined) assert.strictEqual(f.on, on);
  reply(f.id, err ? { error: err } : {});
  try { await p; return true; } catch (e) { assert.strictEqual(e, err); return false; }
}

test('mlstats updates the overlay and is not passed to handleMessage', async function () {
  var h = handled.length;
  listener(STATS);                                  // off: nothing shown
  assert.strictEqual(byId.mlstats, undefined);
  assert.ok(await toggleStats(undefined, 'true'));  // on: the overlay appears with the first event
  assert.strictEqual(byId.mlstats, undefined);
  listener(STATS);
  var el = byId.mlstats;
  assert.ok(el && el.style.display !== 'none' && el.id === 'mlstats');
  assert.strictEqual(el.textContent.split('\n')[0], 'depth 12 ms (audio 8 ms)');
  assert.ok(/play 12\.3 s/.test(el.textContent), el.textContent);
  assert.strictEqual(handled.length, h);
});

test('a second toggle hides the overlay', async function () {
  assert.ok(await toggleStats(undefined, 'false'));
  assert.strictEqual(byId.mlstats.style.display, 'none');
  listener(STATS);                                  // hidden: stays hidden
  assert.strictEqual(byId.mlstats.style.display, 'none');
});

test('streamTerminated hides and clears the overlay, still reaches handleMessage', async function () {
  assert.ok(await toggleStats());
  listener(STATS);
  assert.notStrictEqual(byId.mlstats.textContent, '');
  var h = handled.length;
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  assert.strictEqual(byId.mlstats.style.display, 'none');
  assert.strictEqual(byId.mlstats.textContent, '');
  assert.strictEqual(handled.length, h + 1);
  assert.ok(await toggleStats(undefined, 'true'));  // toggle state was reset: this is "on" again
  listener(STATS);
  assert.notStrictEqual(byId.mlstats.style.display, 'none');
  assert.ok(await toggleStats());                   // leave it off
});

test('a toggle reply after streamTerminated leaves the overlay off', async function () {
  var n = sent.length, p = win.SyncFunctions.toggleStats();
  advance(250);
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  reply(fields(sent[n]).id, {});
  await p;
  listener(STATS);
  assert.strictEqual(byId.mlstats.style.display, 'none');
  assert.ok(await toggleStats(undefined, 'true'));  // the next press turns it on
  assert.ok(await toggleStats(undefined, 'false'));
});

test('a rejected toggleStats leaves the overlay hidden', async function () {
  assert.ok(!(await toggleStats('not supported')));
  assert.strictEqual(byId.mlstats.style.display, 'none');
  var n = sent.length, w = warns.length, p = win.Module.toggleStats();   // yellow key: no unhandled rejection
  advance(250);
  reply(fields(sent[n]).id, { error: 'not supported' });
  assert.strictEqual(await p, undefined);
  assert.ok(warns.slice(w).join('\n').indexOf('not supported') >= 0);
});

function mkpad(id, ts, over) {
  var g = { connected: true, id: id, timestamp: ts, buttons: [], axes: [0, 0, 0, 0] };
  for (var i = 0; i < 17; i++) g.buttons.push({ pressed: false, value: 0 });
  over = over || {};
  for (var b in (over.b || {})) { g.buttons[b].pressed = over.b[b] > 0; g.buttons[b].value = over.b[b]; }
  (over.a || []).forEach(function (v, i) { g.axes[i] = v; });
  return g;
}
function padMsgs() { return sent.map(fields).filter(function (f) { return /^gamepad/.test(f.cmd); }); }

test('gamepad: no polling before Connection Established; placeholder pad is not announced', async function () {
  sent.length = 0; pads = [mkpad('Xbox Wireless Controller', 0)];
  advance(100);
  assert.strictEqual(padMsgs().length, 0);
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(100);
  assert.strictEqual(padMsgs().length, 0, 'timestamp 0 and no input change');
  pads = [mkpad('Xbox Wireless Controller', 0, { b: { 0: 1 } })];   // input change: now real
  advance(8);
  var m = padMsgs();
  assert.strictEqual(m.length, 2);
  assert.strictEqual(m[0].cmd, 'gamepadArrival');
});

test('gamepad: one arrival, state only on change, Y inverted, triggers scaled, no pending or reply', async function () {
  sent.length = 0;
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);   // reset
  var t0 = timers.length;
  pads = [mkpad('Xbox 360 (STANDARD GAMEPAD Vendor: 045e)', 5, { b: { 0: 1, 6: 1, 7: 0.5 }, a: [0.25, 0.25, -1, 1] })];
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(80);
  var m = padMsgs();
  assert.strictEqual(m.length, 2, 'arrival + one state, then no change');
  assert.strictEqual(m[0].cmd, 'gamepadArrival');
  assert.strictEqual(m[0].index, '0'); assert.strictEqual(m[0].mask, '1'); assert.strictEqual(m[0].type, '1');
  assert.strictEqual(m[0].caps, '1'); assert.strictEqual(m[0].buttons, String(0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x300 | 0x30 | 0xC0 | 0xF | 0x400));
  assert.strictEqual(m[1].cmd, 'gamepadState');
  var want = { index: '0', mask: '1', buttons: String(0x1000), lt: '255', rt: '128', lsx: '8192', lsy: '-8192', rsx: '-32767', rsy: '-32767' };
  for (var k in want) assert.strictEqual(m[1][k], want[k], k);
  assert.ok(Object.keys(fields(sent[0])).indexOf('id') >= 0);
  pads[0].axes[0] = 0;
  advance(8);
  assert.strictEqual(padMsgs().length, 3);
  assert.strictEqual(padMsgs()[2].lsx, '0');
  var h = handled.length;
  reply(fields(sent[0]).id, {});            // a reply for a pad id is ignored without error
  assert.strictEqual(handled.length, h);
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
});

test('gamepad: flipAB mapping and removal message', async function () {
  sent.length = 0;
  var a = []; a[16] = true; a[17] = false;
  var p = win.sendMessage('startRequest', a);
  reply(fields(sent[0]).id, {}); await p;
  sent.length = 0;
  pads = [null, mkpad('PS DualSense', 3, { b: { 0: 1 } })];
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(8);
  var m = padMsgs();
  assert.strictEqual(m[0].index, '1'); assert.strictEqual(m[0].mask, '2'); assert.strictEqual(m[0].type, '2');
  assert.strictEqual(m[1].buttons, String(0x2000), 'A sends B');
  pads = [];
  advance(8);
  var r = padMsgs().pop();
  assert.strictEqual(r.cmd, 'gamepadState'); assert.strictEqual(r.index, '1'); assert.strictEqual(r.mask, '0');
  ['buttons', 'lt', 'rt', 'lsx', 'lsy', 'rsx', 'rsy'].forEach(function (k) { assert.strictEqual(r[k], '0', k); });
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
});

test('gamepad: stop combo stops the stream; poll stops after streamTerminated and the next stream announces again', async function () {
  sent.length = 0;
  pads = [mkpad('x', 4, { b: { 8: 1, 9: 1, 4: 1, 5: 1 } })];
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(8);
  var cmds = sent.map(fields).map(function (f) { return f.cmd; });
  assert.ok(cmds.indexOf('stopStream') >= 0, cmds.join());
  assert.strictEqual(padMsgs().filter(function (f) { return f.cmd === 'gamepadState'; }).length, 0, 'combo is not sent as input');
  sent.length = 0; advance(100);
  assert.strictEqual(sent.length, 0, 'polling stopped');
  pads = [mkpad('x', 4)];
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(8);
  assert.strictEqual(padMsgs()[0].cmd, 'gamepadArrival');
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  sent.length = 0; advance(100);
  assert.strictEqual(sent.length, 0);
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(8);
  assert.strictEqual(padMsgs()[0].cmd, 'gamepadArrival', 'announced again');
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  pads = [];
});

test('gamepad: stats combo toggles once per press', async function () {
  sent.length = 0;
  pads = [mkpad('x', 4, { b: { 8: 1, 4: 1, 5: 1, 2: 1 } })];
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(80);
  assert.strictEqual(sent.map(fields).filter(function (f) { return f.cmd === 'toggleStats'; }).length, 1);
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  pads = [];
});

test('gamepad: stick noise is not sent; > 1 % is; keepalive resends the exact state after 32 polls', async function () {
  sent.length = 0;
  pads = [mkpad('x', 5, { b: { 0: 1 }, a: [0.25, 0, 0, 0] })];
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(8 * 2);
  assert.strictEqual(padMsgs().length, 2, 'arrival + state');
  pads[0].axes[0] = 0.2501;   // noise
  advance(8 * 10);
  assert.strictEqual(padMsgs().length, 2, 'noise not sent');
  pads[0].axes[0] = 0.27;     // > 327
  advance(8);
  assert.strictEqual(padMsgs().length, 3);
  assert.strictEqual(padMsgs()[2].lsx, String(Math.round(0.27 * 32767)));
  advance(8 * 30);
  assert.strictEqual(padMsgs().length, 3, 'no keepalive yet');
  advance(8 * 2);
  assert.strictEqual(padMsgs().length, 4, 'keepalive');
  assert.strictEqual(padMsgs()[3].lsx, padMsgs()[2].lsx);
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  pads = [];
});

test('gamepadinputchanged is stopped while polling only', async function () {
  assert.strictEqual(padCapture, true);
  var n = 0, ev = { stopImmediatePropagation: function () { n++; } };
  padListener(ev); assert.strictEqual(n, 0, 'not polling');
  listener([{ key: 'event', value: 'Connection Established' }]);
  padListener(ev); assert.strictEqual(n, 1, 'polling');
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  padListener(ev); assert.strictEqual(n, 1, 'stopped again');
});

test('gamepad: a failing pad send does not drop the port', async function () {
  sent.length = 0; hostDead = true;
  pads = [mkpad('x', 5, { b: { 0: 1 } })];
  listener([{ key: 'event', value: 'Connection Established' }]);
  advance(8 * 3);
  hostDead = false;
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  pads = [];
  var l = launches;
  var p = win.Module.toggleStats(); advance(250);
  assert.ok(sent.map(fields).some(function (f) { return f.cmd === 'toggleStats'; }), 'normal command sent');
  assert.strictEqual(launches, l, 'no reconnect');
  reply(fields(sent[sent.length - 1]).id, {}); await p;
});

function key(code, over) {
  var e = { keyCode: code, key: '', repeat: false, pd: 0, sip: 0, preventDefault: function () { e.pd++; }, stopImmediatePropagation: function () { e.sip++; } };
  for (var k in (over || {})) e[k] = over[k];
  keyListener(e);
  return e;
}
function stops() { return sent.map(fields).filter(function (f) { return f.cmd === 'stopStream'; }).length; }

test('remote Back in game stops the stream once per stream and is consumed', async function () {
  assert.strictEqual(keyCapture, true);
  sent.length = 0; win.isInGame = true;
  var e = key(10009);
  assert.ok(e.pd === 1 && e.sip === 1, 'consumed');
  assert.strictEqual(stops(), 1);
  e = key(10009); assert.ok(e.pd === 1 && e.sip === 1);
  key(10009, { keyCode: 0, key: 'XF86Back', repeat: true });
  assert.strictEqual(stops(), 1, 'no second stop before the next stream');
  listener([{ key: 'event', value: 'Connection Established' }]);
  key(10009, { repeat: true });
  assert.strictEqual(stops(), 1, 'repeat is ignored');
  key(10009);
  assert.strictEqual(stops(), 2, 'stops again after Connection Established');
  win.isInGame = false;
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
});

test('remote Back outside a game and other keys in game pass through', async function () {
  sent.length = 0; win.isInGame = false;
  var e = key(10009);
  assert.ok(e.pd === 0 && e.sip === 0, 'menu navigation keeps working');
  assert.strictEqual(stops(), 0);
  win.isInGame = true;
  e = key(13);
  assert.ok(e.pd === 0 && e.sip === 0);
  assert.strictEqual(stops(), 0);
  win.isInGame = false;
});

test('trace hides secrets',async function () {
  var all = logs.join('\n');
  assert.ok(logs.length > 0);
  ['CERTPEM', 'KEYPEM', 'PPKPEM', '1234'].forEach(function (s) { assert.strictEqual(all.indexOf(s), -1, s); });
});

(async function () {
  var fail = 0;
  for (var i = 0; i < tests.length; i++) {
    try { await tests[i][1](); console.log('ok   ' + tests[i][0]); }
    catch (e) { fail++; console.log('FAIL ' + tests[i][0] + ': ' + (e && e.stack || e)); }
  }
  process.exit(fail ? 1 : 0);
})();
