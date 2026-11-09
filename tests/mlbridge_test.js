// Host self-test for pkg/ui/mlbridge.js: node tests/mlbridge_test.js
var fs = require('fs'), vm = require('vm'), assert = require('assert');

var logs = [], warns = [], errs = [], timers = [], now = 0, tid = 1;
var byId = {}, domReady = null, remoteUp = false, hostDead = false, launchFails = false, launches = 0, sentAt = 0, sent = [], listener = null, handled = [];
var cleared = {}, pads = [];
var padListener = null, padCapture = null;
var winListeners = {};
var win = { addEventListener: function (ev, f, cap) { (winListeners[ev] = winListeners[ev] || []).push(f); if (ev === 'gamepadinputchanged') { padListener = f; padCapture = cap; } }, ML_TRACE: true, handleMessage: function (m) { handled.push(m); } };
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
  assert.strictEqual(f.gameMode, 'false');
  reply(f.id, {});
  await p;
  sent.length = 0;
  var a = []; a[26] = 0;
  p = win.sendMessage('startRequest', a);
  f = fields(sent[0]);
  assert.strictEqual(f.fullRange, 'false');
  assert.strictEqual(f.gameMode, 'false');
  reply(f.id, {});
  await p;
});

test('startRequest gameMode a[27]', async function () {
  sent.length = 0;
  var p = win.sendMessage('startRequest', ['host', 47989, 1920, 1080, 60, 20000, 'abcd', 7,
                                            'v', 'g', 'rtsp://x', 1, 0, 0, 0, 0, 0, 0,
                                            'EMSS', 'Stereo', 1, 40, 1, 1, 'H264', 1, 1, 1]);
  var f = fields(sent[0]);
  assert.strictEqual(f.gameMode, 'true');
  assert.strictEqual(f.fullRange, 'true');
  reply(f.id, {});
  await p;
});

test('startRequest performanceStats a[29] turns the overlay on at Connection Established', async function () {
  for (var want of [true, false, undefined]) {
    var a = ['host', 47989, 1920, 1080, 60, 20000, 'abcd', 7]; a[29] = want;
    sent.length = 0;
    var p = win.sendMessage('startRequest', a);
    reply(fields(sent[0]).id, {}); await p;
    sent.length = 0;
    listener([{ key: 'event', value: 'Connection Established' }]);
    advance(250);
    var t = sent.map(fields).filter(function (f) { return f.cmd === 'toggleStats'; });
    assert.strictEqual(t.length, want ? 1 : 0, String(want));
    if (want) { assert.strictEqual(t[0].on, 'true'); reply(t[0].id, {}); await Promise.resolve(); }
    listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  }
  handled.length = 0;   // 'event passes string' expects only its own event
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

test('mlGameMode sends tvGameMode and logs the rc', async function () {
  sent.length = 0;
  assert.ok(/off/.test(win.mlGameMode(false)));
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'tvGameMode'); assert.strictEqual(f.on, 'false'); assert.ok(!('saveType' in f));
  reply(f.id, { rc: '0' }); await tick();
  assert.ok(logs.indexOf('[mlbridge] TV game mode off -> rc 0') >= 0);
  win.mlGameMode(true, 0);
  f = fields(sent[1]);
  assert.strictEqual(f.on, 'true'); assert.strictEqual(String(f.saveType), '0');
  reply(f.id, { error: 'not supported' }); await tick();
  assert.ok(warns.some(function (w) { return w.indexOf('[mlbridge] TV game mode failed') === 0; }));
});

test('mlStatsOsd sends statsOsd and logs the rc and the window', async function () {
  sent.length = 0;
  assert.ok(/on/.test(win.mlStatsOsd(true)));
  var f = fields(sent[0]);
  assert.strictEqual(f.cmd, 'statsOsd'); assert.strictEqual(f.on, 'true');
  reply(f.id, { rc: '0', win: '1' }); await tick();
  assert.ok(logs.indexOf('[mlbridge] stats over video on -> rc 0 window 1') >= 0);
  win.mlStatsOsd(false);
  f = fields(sent[1]);
  assert.strictEqual(f.on, 'false');
  reply(f.id, { rc: '-1', win: '-1' }); await tick();
  assert.ok(warns.some(function (w) { return w.indexOf('[mlbridge] stats over video off -> rc -1 window -1') === 0; }));
  win.mlStatsOsd(true);
  reply(fields(sent[2]).id, { error: 'not supported' }); await tick();
  assert.ok(warns.some(function (w) { return w.indexOf('[mlbridge] stats over video failed') === 0; }));
});

test('toggleStats logs the osd field of the reply', async function () {
  async function press(rep) {
    var n = sent.length, p = win.SyncFunctions.toggleStats();
    advance(250);
    reply(fields(sent[n]).id, rep); await p;
  }
  var l = logs.length, w = warns.length;
  await press({ osd: '0', osdWin: '1' });   // on
  assert.ok(logs.slice(l).indexOf('[mlbridge] stats over video: osd 0 window 1') >= 0);
  await press({ osd: '-1', osdWin: '0' });  // off
  assert.ok(warns.slice(w).some(function (x) { return /osd -1 window 0.*only after the stream/.test(x); }));
  l = logs.length; w = warns.length;
  await press({});                          // an old host: no osd field, no line
  assert.strictEqual(logs.slice(l).concat(warns.slice(w)).filter(function (x) { return /stats over video/.test(x); }).length, 0);
  await press({});                          // leave it off
});

test('page gone: pagehide then unload send one stopStream, only during a stream', async function () {
  function gone(ev) { (winListeners[ev] || []).forEach(function (f) { f({}); }); }
  function stops() { return sent.map(fields).filter(function (f) { return f.cmd === 'stopStream'; }).length; }
  sent.length = 0; gone('pagehide'); gone('unload');
  assert.strictEqual(stops(), 0, 'no stream');
  pads = [];
  listener([{ key: 'event', value: 'Connection Established' }]);
  gone('pagehide'); gone('unload');
  assert.strictEqual(stops(), 1);
  handled.length = 0;
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

var STATS = [{ key: 'event', value: 'mlstats' }, { key: 'depth', value: '12' }, { key: 'fps', value: '60' },
  { key: 'drop', value: '0' }, { key: 'full', value: '0' }, { key: 'uo', value: '0/0' }, { key: 'err', value: '0' }, { key: 'rec', value: '0' }, { key: 'play', value: '12300' },
  { key: 'asent', value: '190' }, { key: 'adrop', value: '3' }, { key: 'afail', value: '1' }];
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
  assert.strictEqual(el.textContent.split('\n')[0], 'depth 12 ms');
  assert.ok(/play 12\.3 s/.test(el.textContent), el.textContent);
  assert.ok(/\nhost audio sent 190 drop 3 fail 1$/.test(el.textContent), el.textContent);
  assert.strictEqual(handled.length, h);
});

test('mlaroute is not passed to handleMessage; native stops the Web Audio scheduler', async function () {
  var h = handled.length, stopped = 0;
  win.mlAudioStop = function () { stopped++; };
  win.mlAudioStats = function () { return { received: 1, scheduled: 1, dropped: 0, empty: 0, resyncs: 0, underruns: 0, headroomMs: 40 }; };
  listener([{ key: 'event', value: 'mlaroute' }, { key: 'route', value: 'webaudio' }]);
  assert.strictEqual(stopped, 0);                   // webaudio: the scheduler keeps running
  assert.strictEqual(handled.length, h);
  listener([{ key: 'event', value: 'mlstats' }].concat(STATS.slice(1)));   // web audio counters still shown
  assert.ok(/web audio rx 1/.test(byId.mlstats.textContent), byId.mlstats.textContent);
  listener([{ key: 'event', value: 'mlaroute' }, { key: 'route', value: 'native' }]);
  assert.strictEqual(stopped, 1);                   // native: the scheduler is stopped
  assert.strictEqual(handled.length, h);
  var NATIVE = [{ key: 'event', value: 'mlstats' }, { key: 'depth', value: '12' }, { key: 'fps', value: '60' },
    { key: 'drop', value: '0' }, { key: 'full', value: '0' }, { key: 'uo', value: '0/0' }, { key: 'err', value: '0' }, { key: 'rec', value: '0' }, { key: 'play', value: '12300' },
    { key: 'aroute', value: 'native' }, { key: 'aframes', value: '990' }, { key: 'auo', value: '1/2' }, { key: 'adrops', value: '3' }, { key: 'abuf', value: '50' }];
  listener(NATIVE);                                 // native counters replace the web audio lines
  assert.ok(/audio native  buf 50 ms  frames 990/.test(byId.mlstats.textContent), byId.mlstats.textContent);
  assert.ok(/under\/over 1\/2  dropped 3/.test(byId.mlstats.textContent), byId.mlstats.textContent);
  assert.ok(!/web audio rx/.test(byId.mlstats.textContent), byId.mlstats.textContent);
  listener([{ key: 'event', value: 'mlaroute' }, { key: 'route', value: 'bogus' }]);   // unknown: ignored
  listener(NATIVE);
  assert.ok(/audio native/.test(byId.mlstats.textContent));   // the bogus route changed nothing: native stats still render
  delete win.mlAudioStop; delete win.mlAudioStats;
  // leave the overlay on: the next test toggles it off (as it did before this test existed)
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

// --- mlaudio: events to the Web Audio feed, and the scheduler itself ---
function bytes(n) { var a = []; for (var i = 0; i < n; i++) a.push(i & 255); return a; }
test('mlaudio: small event reaches mlAudioFeed as an ArrayBuffer, not handleMessage', async function () {
  var calls = [], h = handled.length, l = logs.length;
  win.mlAudioFeed = function (r, c, n, b) { calls.push([r, c, n, b]); };
  listener([{ key: 'event', value: 'mlaudio' }, { key: 'rate', value: '48000' }, { key: 'ch', value: '2' }, { key: 'samples', value: '240' }, { key: 'data', value: bytes(960) }]);
  assert.strictEqual(calls.length, 1);
  assert.deepStrictEqual(calls[0].slice(0, 3), ['48000', '2', '240']);
  assert.ok(calls[0][3] instanceof ArrayBuffer); assert.strictEqual(calls[0][3].byteLength, 960);
  assert.strictEqual(new Uint8Array(calls[0][3])[300], 300 & 255);
  assert.strictEqual(handled.length, h); assert.strictEqual(logs.length, l, 'no per-event log');
});

test('mlaudio: a big event sent as parts is fed once, joined', async function () {
  var calls = [], h = handled.length, all = bytes(2880);
  win.mlAudioFeed = function (r, c, n, b) { calls.push([r, c, n, b]); };
  var parts = [['event', 'mlaudio'], ['rate', '48000'], ['ch', '6'], ['samples', '240']].map(function (kv) { return [kv[0], kv[1]]; });
  var chunks = [all.slice(0, 1536), all.slice(1536)];
  var list = parts.map(function (p) { return { field: p[0], chunk: p[1] }; }).concat(chunks.map(function (c) { return { field: 'data', chunk: c }; }));
  list.forEach(function (p, i) {
    listener([{ key: 'id', value: 'ev9' }, { key: 'seq', value: String(i) }, { key: 'total', value: String(list.length) }, { key: 'field', value: p.field }, { key: 'chunk', value: p.chunk }]);
    assert.strictEqual(calls.length, i === list.length - 1 ? 1 : 0);
  });
  assert.deepStrictEqual(calls[0].slice(0, 3), ['48000', '6', '240']);
  assert.strictEqual(calls[0][3].byteLength, 2880);
  assert.strictEqual(handled.length, h);
});

test('mlaudio: startStream starts and streamTerminated / stopStream stop the scheduler; overlay shows counters', async function () {
  var ev = [];
  win.mlAudioStart = function () { ev.push('start'); };
  win.mlAudioStop = function () { ev.push('stop'); };
  win.mlAudioStats = function () { return { received: 7, scheduled: 6, dropped: 1, empty: 0, resyncs: 2, underruns: 1, headroomMs: 99.6 }; };
  sent.length = 0;
  var p = win.sendMessage('startRequest', ['host', 47989, 1920, 1080, 60, 20000, 'abcd', 7]);
  assert.deepStrictEqual(ev, ['start']);
  reply(fields(sent[0]).id, {}); await p;
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  assert.deepStrictEqual(ev, ['start', 'stop']);
  var q = win.sendMessage('stopRequest', []);
  assert.deepStrictEqual(ev, ['start', 'stop', 'stop']);
  reply(fields(sent[sent.length - 1]).id, {}); await q;
  await toggleStats(undefined, 'true');
  listener(STATS);
  assert.ok(/web audio rx 7 sched 6 drop 1/.test(byId.mlstats.textContent) && /headroom 100 ms/.test(byId.mlstats.textContent));
  await toggleStats(undefined, 'false');
  delete win.mlAudioFeed; delete win.mlAudioStart; delete win.mlAudioStop; delete win.mlAudioStats;
});

test('mlaudio: audio.js globals defined after mlaudio.js do not replace our scheduler', async function () {
  var ctxs = 0, fed = 0, sbw = { AudioContext: function () { ctxs++; this.sampleRate = 48000; this.state = 'running'; this.currentTime = 0; this.destination = {}; this.close = function () {};
    this.createBuffer = function (ch, n, r) { var d = []; for (var c = 0; c < ch; c++) d.push(new Float32Array(n)); return { duration: n / r, getChannelData: function (c) { return d[c]; } }; };
    this.createBufferSource = function () { return { connect: function () {}, start: function () { fed++; } }; }; } };
  vm.runInNewContext(fs.readFileSync(__dirname + '/../pkg/ui/mlaudio.js', 'utf8'),
    { window: sbw, console: { log: function () {}, warn: function () {} }, Date: Date, Int16Array: Int16Array, Float32Array: Float32Array, Math: Math });
  var fork = 0;
  sbw.startAudioScheduler = function () { fork++; }; sbw.stopAudioScheduler = function () { fork++; };   // as the original audio.js
  assert.strictEqual(typeof sbw.mlAudioStart, 'function');
  sbw.mlAudioStart();
  sbw.mlAudioFeed(48000, 2, 240, new ArrayBuffer(960));
  assert.strictEqual(ctxs, 1); assert.strictEqual(fed, 1); assert.strictEqual(sbw.mlAudioStats().running, true); assert.strictEqual(fork, 0);
  // the bridge calls our names, not the globals audio.js defines
  var ev = [];
  win.startAudioScheduler = function () { ev.push('fork'); }; win.stopAudioScheduler = function () { ev.push('fork'); };
  win.mlAudioStart = function () { ev.push('start'); }; win.mlAudioStop = function () { ev.push('stop'); };
  sent.length = 0;
  var p = win.sendMessage('startRequest', ['host', 47989, 1920, 1080, 60, 20000, 'abcd', 7]);
  reply(fields(sent[0]).id, {}); await p;
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  assert.deepStrictEqual(ev, ['start', 'stop']);
  delete win.startAudioScheduler; delete win.stopAudioScheduler; delete win.mlAudioStart; delete win.mlAudioStop;
});

test('mlaudio.js: conversion, priming, drop above 2x target, resampling, no AudioContext', async function () {
  var wlogs = [], wwarns = [], T = 1, state = 'running', made = [], closed = 0, resumed = 0, rate = 48000;
  function FakeCtx() {
    this.sampleRate = rate; this.state = state; this.destination = {};
    this.__defineGetter__('currentTime', function () { return T; });
    this.createBuffer = function (ch, n, r) { var d = []; for (var c = 0; c < ch; c++) d.push(new Float32Array(n)); return { duration: n / r, getChannelData: function (c) { return d[c]; } }; };
    this.createBufferSource = function () { var s = { connect: function () {}, start: function (t) { s.at = t; made.push(s); } }; return s; };
    this.close = function () { closed++; };
    this.resume = function () { resumed++; };
  }
  var w = { AudioContext: FakeCtx };
  var sb = { window: w, console: { log: function () { wlogs.push([].join.call(arguments, ' ')); }, warn: function () { wwarns.push([].join.call(arguments, ' ')); } }, Date: Date, Int16Array: Int16Array, Float32Array: Float32Array, Math: Math };
  vm.runInNewContext(fs.readFileSync(__dirname + '/../pkg/ui/mlaudio.js', 'utf8'), sb);
  function frame(n, ch, fn) { var a = new Int16Array(n * ch); for (var i = 0; i < a.length; i++) a[i] = fn(i); return a.buffer; }
  w.mlAudioStart();
  // S16 -> float, de-interleave: L = 16384, R = -32768
  w.mlAudioFeed('48000', '2', '240', frame(240, 2, function (i) { return i % 2 ? -32768 : 16384; }));
  assert.strictEqual(made.length, 1);
  assert.strictEqual(made[0].buffer.getChannelData(0)[5], 0.5); assert.strictEqual(made[0].buffer.getChannelData(1)[5], -1);
  assert.ok(Math.abs(made[0].at - 1.1) < 1e-9, 'primed to the 100 ms jitter buffer');
  // the next frame follows the first one
  w.mlAudioFeed(48000, 2, 240, frame(240, 2, function () { return 0; }));
  assert.ok(Math.abs(made[1].at - (1.1 + 0.005)) < 1e-9);
  // feed until the schedule is over 2x target ahead: dropped, not scheduled
  for (var i = 0; i < 100; i++) w.mlAudioFeed(48000, 2, 240, frame(240, 2, function () { return 0; }));
  var st = w.mlAudioStats();
  assert.ok(st.dropped > 0 && st.scheduled < 102 && st.scheduled + st.dropped === st.received);
  assert.ok(made[made.length - 1].at <= 1.2 + 0.005 + 1e-9);
  // an underrun primes to the whole buffer again
  T = 5;
  w.mlAudioFeed(48000, 2, 240, frame(240, 2, function () { return 0; }));
  assert.ok(Math.abs(made[made.length - 1].at - 5.1) < 1e-9);
  assert.strictEqual(w.mlAudioStats().underruns, 1);
  // a suspended context is resumed, the frame is not played
  var n = made.length; state = 'suspended';
  w.mlAudioStop(); w.mlAudioStart();
  w.mlAudioFeed(48000, 2, 240, frame(240, 2, function () { return 0; }));
  assert.strictEqual(made.length, n); assert.ok(resumed >= 1); assert.strictEqual(closed, 1);
  // a context at 44100 Hz: resampled, total duration stays close
  state = 'running'; rate = 44100; made.length = 0;
  w.mlAudioStop(); w.mlAudioStart();
  var dur = 0;
  for (i = 0; i < 20; i++) { w.mlAudioFeed(48000, 2, 240, frame(240, 2, function (k) { return k; })); }
  made.forEach(function (s) { dur += s.buffer.duration; });
  assert.ok(Math.abs(dur - 20 * 0.005) < 0.001, 'resampled duration ' + dur);
  assert.ok(wwarns.some(function (m) { return /resampling/.test(m); }));
  w.mlAudioStop();
  assert.strictEqual(closed, 3);
  w.mlAudioFeed(48000, 2, 240, frame(240, 2, function () { return 0; }));   // stopped: no-op
  // no AudioContext: warn once, feed is a no-op
  var w2 = {}, warns2 = [];
  vm.runInNewContext(fs.readFileSync(__dirname + '/../pkg/ui/mlaudio.js', 'utf8'),
    { window: w2, console: { log: function () {}, warn: function (m) { warns2.push(m); } }, Date: Date });
  w2.mlAudioStart(); w2.mlAudioStart();
  w2.mlAudioFeed(48000, 2, 240, new ArrayBuffer(960));
  assert.deepStrictEqual(warns2, ['audio output unavailable (Web Audio)']);
});

test('mlaudio: partial au frames expire (max 8) and clear on stop; other ids and traces unaffected', async function () {
  var calls = [], l = logs.length;
  win.mlAudioFeed = function (r, c, n, b) { calls.push(b); };
  function part(id, seq, total, field, chunk) { listener([{ key: 'id', value: id }, { key: 'seq', value: String(seq) }, { key: 'total', value: String(total) }, { key: 'field', value: field }, { key: 'chunk', value: chunk }]); }
  function first(id) { part(id, 0, 5, 'event', 'mlaudio'); }   // the rest never arrives
  function full(id) { [['event', 'mlaudio'], ['rate', '48000'], ['ch', '6'], ['samples', '240'], ['data', bytes(1536)]].forEach(function (p, i) { part(id, i, 6, p[0], p[1]); }); part(id, 5, 6, 'data', bytes(1344)); }
  first('xx1');   // other id: stays pending
  for (var i = 1; i <= 9; i++) first('au' + i);   // au1 is expired by au9
  full('au100');
  assert.strictEqual(calls.length, 1); assert.strictEqual(calls[0].byteLength, 2880);
  assert.strictEqual(new Uint8Array(calls[0])[1536], 0); assert.strictEqual(new Uint8Array(calls[0])[1535], 255);
  assert.ok(!logs.slice(l).join('\n').match(/joined id=au/), 'no join trace for au');
  // au9 is still pending: completing it works; after a stop it does not
  [['rate', '48000'], ['ch', '2'], ['samples', '240']].forEach(function (p, i) { part('au9', i + 1, 5, p[0], p[1]); });
  part('au9', 4, 5, 'data', bytes(960));
  assert.strictEqual(calls.length, 2);
  first('au20'); win.mlAudioStop = function () {};
  win.mlAudioFeed = function () { calls.push('x'); };
  listener([{ key: 'event', value: 'streamTerminated: 0' }]);
  [['rate', '48000'], ['ch', '2'], ['samples', '240']].forEach(function (p, i) { part('au20', i + 1, 5, p[0], p[1]); });
  part('au20', 4, 5, 'data', bytes(960));
  assert.strictEqual(calls.length, 2, 'au20 was cleared at streamTerminated');
  // the other id survived the stream end and the au expiry
  [['rate', '48000'], ['ch', '2'], ['samples', '240']].forEach(function (p, i) { part('xx1', i + 1, 5, p[0], p[1]); });
  part('xx1', 4, 5, 'data', bytes(960));
  assert.strictEqual(calls.length, 3);
  assert.ok(logs.slice(l).join('\n').indexOf('joined id=xx1') >= 0, 'other ids keep the trace');
  delete win.mlAudioFeed; delete win.mlAudioStop;
});

test('mlaudio.js: resume at most once per second, rejection handled, second start keeps the context', async function () {
  var T = 0, made = 0, resumed = 0, warns = [], handledRej = 0;
  function FakeCtx() {
    made++; this.sampleRate = 48000; this.state = 'suspended'; this.destination = {}; this.currentTime = 0;
    this.close = function () {};
    this.resume = function () { resumed++; return { catch: function (f) { handledRej++; f('denied'); } }; };
  }
  var w = { AudioContext: FakeCtx };
  vm.runInNewContext(fs.readFileSync(__dirname + '/../pkg/ui/mlaudio.js', 'utf8'),
    { window: w, console: { log: function () {}, warn: function (m) { warns.push(m); } }, Date: { now: function () { return T; } }, Int16Array: Int16Array, Float32Array: Float32Array, Math: Math });
  w.mlAudioStart(); w.mlAudioStart();
  assert.strictEqual(made, 1, 'second start is a no-op');
  assert.strictEqual(resumed, 1);
  for (var i = 0; i < 50; i++) { T += 5; w.mlAudioFeed(48000, 2, 240, new ArrayBuffer(960)); }
  assert.strictEqual(resumed, 1, 'throttled');
  T += 1000; w.mlAudioFeed(48000, 2, 240, new ArrayBuffer(960));
  assert.strictEqual(resumed, 2);
  assert.strictEqual(handledRej, 2); assert.strictEqual(warns.length, 1, 'warned once');
  assert.strictEqual(w.mlAudioStats().resumes, 2);
});

test('trace hides secrets', async function () {
  var all = logs.join('\n');
  assert.ok(logs.length > 0);
  ['CERTPEM', 'KEYPEM', 'PPKPEM', '1234'].forEach(function (s) { assert.strictEqual(all.indexOf(s), -1, s); });
});

// Persistent store (issue #21): a second context with an in-memory wgt-private and IndexedDB.
function mkStore(o) {
  o = o || {};
  var e = { idb: o.idb || {}, files: {}, ls: {}, logs: [], warns: [], timers: [], now: 0, started: 0, held: [], opens: 0, maxOpen: 0, open: 0, moves: [], origGet: null, origStore: null };
  if (o.file !== undefined) e.files['mlstore.json'] = o.file;
  function mkfile(n) { return { fullPath: 'wgt-private/' + n, readAsText: function (ok) { if (!o.hang) ok(e.files[n]); }, name: n }; }
  var dir = {
    fullPath: 'wgt-private',
    resolve: function (n) { if (e.files[n] === undefined) throw new Error('NotFound'); var f = mkfile(n);
      f.openStream = function (m, ok) { assert.strictEqual(m, 'w'); e.files[n] = ''; e.open++; e.maxOpen = Math.max(e.maxOpen, e.open);
        ok({ write: function (x) { e.files[n] += x; }, close: function () {} }); };
      return f; },
    createFile: function (n) { e.files[n] = ''; },
    moveTo: function (a, b, ow, ok) { assert.strictEqual(ow, true); e.moves.push([a, b]);
      e.held.push(function () { e.files[b.split('/')[1]] = e.files[a.split('/')[1]]; delete e.files[a.split('/')[1]]; e.open--; ok(); }); }
  };
  e.flush = function () { while (e.held.length) e.held.shift()(); };
  var g = e.origGet = function (k, cb) { var r = {}; r[k] = e.idb[k] === undefined ? null : JSON.parse(e.idb[k]); cb(r); };
  var st = e.origStore = function (k, d, cb) { e.idb[k] = JSON.stringify(d); if (cb) cb(); };
  var w = { ML_TRACE: false, common: { domContentLoaded: function () {} }, moduleDidLoad: function () { e.started++; } };
  if (!o.nogd) { w.getData = g; w.storeData = st; }
  var sb = {
    window: w, Promise: Promise, Uint8Array: Uint8Array, ArrayBuffer: ArrayBuffer,
    tizen: { application: { getCurrentApplication: function () { return { appInfo: { id: 'Moonlight5.ui', packageId: 'Moonlight5' } }; },
      getAppInfo: function () { return { id: 'x', version: '1', packageId: 'p' }; }, getAppsContext: function (ok) { ok([]); },
      launch: function (i, ok) { ok(); } },
      messageport: { requestLocalMessagePort: function () { return { addMessagePortListener: function () {} }; }, requestRemoteMessagePort: function () { throw new Error('x'); } } },
    localStorage: { getItem: function (k) { return e.ls[k] === undefined ? null : e.ls[k]; }, setItem: function (k, v) { e.ls[k] = String(v); } },
    document: { addEventListener: function (ev, f) { if (ev === 'DOMContentLoaded') e.dom = f; }, body: { appendChild: function () {} },
      createElement: function () { return { style: {} }; }, getElementById: function () { return null; } },
    console: { log: function () { e.logs.push([].join.call(arguments, ' ')); }, warn: function () { e.warns.push([].join.call(arguments, ' ')); }, error: function () {} },
    setTimeout: function (f, ms) { e.timers.push({ at: e.now + ms, f: f }); return e.timers.length; },
    clearTimeout: function () {}, setInterval: function () { return 0; }, clearInterval: function () {},
    navigator: { getGamepads: function () { return []; } }
  };
  if (!o.nofs) sb.tizen.filesystem = { resolve: function (loc, ok, err, mode) { assert.strictEqual(loc, 'wgt-private'); assert.strictEqual(mode, 'rw'); ok(dir); } };
  vm.runInNewContext(fs.readFileSync(__dirname + '/../pkg/ui/mlbridge.js', 'utf8'), sb);
  e.w = w;
  e.boot = function () { e.dom(); w.common.domContentLoaded(); };
  e.advance = function (ms) { e.now += ms; e.timers.filter(function (t) { return t.at <= e.now; }).forEach(function (t) { t.at = Infinity; t.f(); }); };
  e.get = function (k) { var r; w.getData(k, function (o) { r = o[k]; }); return r; };
  e.file = function () { return JSON.parse(e.files['mlstore.json']); };
  e.diag = function () { return e.logs.filter(function (l) { return l.indexOf('[mlbridge] storage:') === 0; }); };
  return e;
}
var H1 = { u1: { serverUid: 'u1', hostname: 'SECRETNAME1', address: '10.9.9.1' } };
var H2 = { u2: { serverUid: 'u2', hostname: 'SECRETNAME2', address: '10.9.9.2' } };
function js(x) { return JSON.stringify(x); }

test('store: no file, IndexedDB hosts are used and written to the file', function () {
  var e = mkStore({ idb: { hosts: js(H1) } }); e.boot();
  assert.strictEqual(e.started, 1);
  assert.strictEqual(js(e.get('hosts')), js(H1));
  assert.deepStrictEqual(e.moves, [['wgt-private/mlstore.json.tmp', 'wgt-private/mlstore.json']]);
  e.flush(); assert.deepStrictEqual(e.file(), { hosts: H1 });
  assert.deepStrictEqual(e.diag(), ['[mlbridge] storage: app Moonlight5.ui package Moonlight5, hosts in IndexedDB 1, in file 0, used indexeddb, file missing, merged 0']);
});

test('store: file hosts and empty IndexedDB (issue #21)', function () {
  var e = mkStore({ file: js({ hosts: H1 }) }); e.boot();
  assert.strictEqual(js(e.get('hosts')), js(H1));
  assert.deepStrictEqual(JSON.parse(e.idb.hosts), H1);
  assert.ok(/hosts in IndexedDB 0, in file 1, used file, file ok, merged 0/.test(e.diag()[0]));
  assert.strictEqual(e.diag().length, 1);
  e.get('hosts'); assert.strictEqual(e.diag().length, 1, 'one line only');
});

test('store: merge once by serverUid, file entries win', function () {
  var fileH = { u1: { serverUid: 'u1', hostname: 'FILE' } };
  var idbH = { u1: { serverUid: 'u1', hostname: 'IDB' }, u2: H2.u2 };
  var e = mkStore({ file: js({ hosts: fileH }), idb: { hosts: js(idbH) } }); e.boot();
  var h = e.get('hosts');
  assert.strictEqual(Object.keys(h).sort().join(), 'u1,u2'); assert.strictEqual(h.u1.hostname, 'FILE');
  assert.deepStrictEqual(Object.keys(JSON.parse(e.idb.hosts)).sort(), ['u1', 'u2']);
  e.flush(); assert.deepStrictEqual(Object.keys(e.file().hosts).sort(), ['u1', 'u2']);
  assert.strictEqual(e.ls.mlstoreHostsMerged, '1');
  assert.ok(/merged 1/.test(e.diag()[0]));
  // second launch: flag set, a host deleted in the file stays deleted
  var e2 = mkStore({ file: js({ hosts: fileH }), idb: { hosts: js(idbH) } }); e2.ls.mlstoreHostsMerged = '1'; e2.boot();
  assert.strictEqual(Object.keys(e2.get('hosts')).join(), 'u1');
  assert.ok(/merged 0/.test(e2.diag()[0]));
});

test('store: storeData updates IndexedDB and file, one write in flight', function () {
  var e = mkStore({ file: js({ resolution: 1 }) }); e.boot();
  e.w.storeData('resolution', 2); e.w.storeData('resolution', 3); e.w.storeData('resolution', 4);
  assert.strictEqual(e.idb.resolution, '4');
  assert.strictEqual(e.maxOpen, 1);
  e.flush(); e.flush();
  assert.strictEqual(e.maxOpen, 1);
  assert.strictEqual(e.file().resolution, 4);
  assert.strictEqual(e.held.length, 0);
});

test('store: bad JSON file is ignored and overwritten', function () {
  var e = mkStore({ file: '{oops', idb: { hosts: js(H1) } }); e.boot();
  assert.strictEqual(js(e.get('hosts')), js(H1));
  assert.ok(/file bad json/.test(e.diag()[0]));
  e.flush(); assert.deepStrictEqual(e.file(), { hosts: H1 });
});

test('store: a read that never answers does not block the UI', function () {
  var e = mkStore({ file: '{}', hang: true }); e.boot();
  assert.strictEqual(e.started, 0);
  e.advance(1999); assert.strictEqual(e.started, 0);
  e.advance(1); assert.strictEqual(e.started, 1);
  assert.ok(e.warns.some(function (w) { return w.indexOf('[mlbridge] storage:') === 0; }));
});

test('store: no tizen.filesystem keeps the plain behaviour', function () {
  var e = mkStore({ nofs: true });
  var g = e.w.getData, s = e.w.storeData; e.boot();
  assert.strictEqual(e.started, 1); assert.strictEqual(e.w.getData, g); assert.strictEqual(e.w.storeData, s);
  e = mkStore({ nogd: true }); e.boot(); assert.strictEqual(e.started, 1);
});

test('store: the diagnostic line holds no stored values', function () {
  var e = mkStore({ file: js({ hosts: H1, cert: 'CERTVALUE' }), idb: { hosts: js(H2) } }); e.boot();
  e.get('hosts'); e.get('cert');
  var all = e.logs.concat(e.warns).join('\n');
  ['SECRETNAME', '10.9.9', 'CERTVALUE', 'u1', 'u2'].forEach(function (x) { assert.strictEqual(all.indexOf(x), -1, x); });
  assert.ok(e.diag().length === 1);
});

(async function () {
  var fail = 0;
  for (var i = 0; i < tests.length; i++) {
    try { await tests[i][1](); console.log('ok   ' + tests[i][0]); }
    catch (e) { fail++; console.log('FAIL ' + tests[i][0] + ': ' + (e && e.stack || e)); }
  }
  process.exit(fail ? 1 : 0);
})();
