/* eslint-disable */
/*
 * mlaudio.js - Web Audio output for the Moonlight Tizen 5.0 web UI.
 *
 * Port of moonlight-tizen's wasm/platform/audio.js scheduler. The TV player cannot play
 * audio fed by the engine, so the host sends decoded PCM in `mlaudio` events (see
 * pkg/ui/PROTOCOL.md) and mlbridge.js hands each frame to window.mlAudioFeed().
 * Kept from audio.js: AudioContext at 48 kHz / 'interactive' with fallbacks, jitter buffer
 * (window._mlAudioTargetMs, default 100 ms), scheduling on a next-time cursor against
 * context.currentTime, priming to the full jitter buffer after an underrun, drop when the
 * schedule ran away > 2x the target, stateful resampling when the context rate differs,
 * resume() of a suspended context, statistics every 5 s. Removed: WASM heap access, slots,
 * sequence gap detection, EMSS backend.
 * One feed call = one host event (a batch of up to ML_AUDIO_BATCH_MS of audio, see PROTOCOL.md): `received` counts
 * events, `audio=` is the audio ms received in the window (5000 ms per 5 s window = no loss).
 * API: mlAudioStart(), mlAudioStop(), mlAudioFeed(rate, ch, samples, buffer),
 * mlAudioStats().
 * The names differ from audio.js on purpose: the original audio.js defines startAudioScheduler /
 * stopAudioScheduler as globals and is loaded after this file, so it would overwrite them.
 */
(function () {
  var STATS_MS = 5000, DEFAULT_TARGET_MS = 100;
  var ctx = null, nextTime = 0, running = false, streamRate = 0, ratio = 1, phase = 0, tail = null;
  var st = null, tot = null, warned = false, lastResume = -Infinity, resumeWarned = false;

  function resetStats() {
    st = { since: Date.now(), received: 0, rxMs: 0, scheduled: 0, droppedFull: 0, droppedEmpty: 0, notRunning: 0, resyncs: 0, underruns: 0, resumes: 0,
           hMin: Infinity, hMax: -Infinity, hSum: 0, hist: [0, 0, 0, 0, 0, 0] };
  }
  function resetTotals() { tot = { received: 0, scheduled: 0, dropped: 0, empty: 0, idle: 0, resyncs: 0, underruns: 0, resumes: 0, headroomMs: 0 }; }
  resetStats(); resetTotals();

  function fmt(v) { return isFinite(v) ? v.toFixed(1) : 'n/a'; }
  function targetSeconds() { return (window._mlAudioTargetMs || DEFAULT_TARGET_MS) / 1000; }

  function logStats(force) {
    if (!force && Date.now() - st.since < STATS_MS) return;
    var avg = st.scheduled > 0 ? st.hSum / st.scheduled : 0, h = st.hist;
    console.log('[mlaudio, statistics] rate=' + streamRate + '/' + (ctx ? ctx.sampleRate : 0) + ' Hz ratio=' + ratio.toFixed(4) +
      ' | latency base=' + fmt(ctx ? ctx.baseLatency * 1000 : NaN) + ' ms output=' + fmt(ctx && ctx.outputLatency ? ctx.outputLatency * 1000 : NaN) + ' ms' +
      ' | frames received=' + st.received + ' audio=' + Math.round(st.rxMs) + ' ms scheduled=' + st.scheduled + ' dropped=' + st.droppedFull + ' empty=' + st.droppedEmpty + ' idle=' + st.notRunning +
      ' | resync=' + st.resyncs + ' underrun=' + st.underruns + ' resume=' + st.resumes +
      ' | headroom min=' + fmt(st.hMin) + ' avg=' + fmt(avg) + ' max=' + fmt(st.hMax) + ' ms' +
      ' | histogram <0=' + h[0] + ' 0-10=' + h[1] + ' 10-25=' + h[2] + ' 25-50=' + h[3] + ' 50-100=' + h[4] + ' 100+=' + h[5]);
    resetStats();
  }

  function recordHeadroom(sec) {
    var ms = sec * 1000;
    st.hSum += ms; tot.headroomMs = ms;
    if (ms < st.hMin) st.hMin = ms;
    if (ms > st.hMax) st.hMax = ms;
    st.hist[ms < 0 ? 0 : ms < 10 ? 1 : ms < 25 ? 2 : ms < 50 ? 3 : ms < 100 ? 4 : 5]++;
  }

  // At most one resume() per second; a rejected promise is caught (warned once).
  function tryResume() {
    var now = Date.now();
    if (now - lastResume < 1000) return;
    lastResume = now; st.resumes++; tot.resumes++;
    function warn(e) { if (!resumeWarned) { resumeWarned = true; console.warn('[mlaudio] resume failed: ' + e); } }
    try { var p = ctx.resume(); if (p && typeof p.catch === 'function') p.catch(warn); } catch (e) { warn(e); }
  }

  // Must run from the startStream path (user gesture): the autoplay policy blocks other contexts.
  window.mlAudioStart = function () {
    if (running) return;   // a second startStream: keep the context
    window.mlAudioStop();
    var AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) {
      if (!warned) { warned = true; console.warn('audio output unavailable (Web Audio)'); }
      return;
    }
    try { ctx = new AC({ sampleRate: 48000, latencyHint: 'interactive' }); }
    catch (e) {
      console.warn('[mlaudio] falling back to the default audio context options: ' + e);
      try { ctx = new AC(); } catch (e2) { ctx = null; if (!warned) { warned = true; console.warn('audio output unavailable (Web Audio)'); } return; }
    }
    nextTime = 0; running = true; streamRate = 0; ratio = 1; phase = 0; tail = null;
    resetStats(); resetTotals();
    // The TV may suspend or reopen the output (e.g. Game Mode): prime again when it comes back.
    ctx.onstatechange = function () { console.log('[mlaudio] audio context state: ' + (ctx && ctx.state)); nextTime = 0; };
    lastResume = -Infinity;
    if (ctx.state === 'suspended') tryResume();
    console.log('[mlaudio] started: ' + ctx.sampleRate + ' Hz, base latency ' + fmt((ctx.baseLatency || 0) * 1000) + ' ms, jitter buffer ' + targetSeconds() * 1000 + ' ms');
  };

  // Like audio.js: log the last statistics and close the context (this stops scheduled sources).
  window.mlAudioStop = function () {
    if (ctx) logStats(true);
    running = false; nextTime = 0; streamRate = 0; ratio = 1; phase = 0; tail = null;
    if (ctx) {
      try { ctx.onstatechange = null; ctx.close(); }
      catch (e) { console.warn('[mlaudio] failed to close the audio context: ' + e); }
      ctx = null;
    }
  };

  window.mlAudioStats = function () {
    return { received: tot.received, scheduled: tot.scheduled, dropped: tot.dropped, empty: tot.empty, idle: tot.idle, resumes: tot.resumes,
             resyncs: tot.resyncs, underruns: tot.underruns, headroomMs: tot.headroomMs, running: running };
  };

  // S16LE interleaved -> planar float, resampled (linear, phase and tail carried over) when the
  // context rate differs. Returns the buffer, or null when no sample came out.
  function build(pcm, samples, ch) {
    var c, i, n = ratio === 1 ? samples : Math.max(0, Math.ceil((samples - 1 - phase) / ratio));
    var buf = n > 0 ? ctx.createBuffer(ch, n, ctx.sampleRate) : null;
    if (ratio === 1) {   // here n = samples > 0
      for (c = 0; c < ch; c++) {
        var out = buf.getChannelData(c);
        for (i = 0; i < samples; i++) out[i] = pcm[i * ch + c] / 32768;
      }
      return buf;
    }
    if (!tail || tail.length !== ch) tail = new Float32Array(ch);
    for (c = 0; c < ch; c++) {
      var o = buf && buf.getChannelData(c), pos = phase;
      for (i = 0; buf && i < n; i++) {
        var w = Math.floor(pos), f = pos - w;
        var a = w < 0 ? tail[c] : pcm[w * ch + c] / 32768, b = pcm[(w + 1) * ch + c] / 32768;
        o[i] = a + (b - a) * f;
        pos += ratio;
      }
      tail[c] = pcm[(samples - 1) * ch + c] / 32768;
    }
    phase = phase + n * ratio - samples;
    return buf;
  }

  window.mlAudioFeed = function (rate, ch, samples, data) {
    if (!running || !ctx) return;
    rate = +rate; ch = +ch; samples = +samples;
    if (!(rate > 0 && ch > 0 && samples > 0) || !data || data.byteLength < samples * ch * 2) return;
    st.received++; tot.received++; st.rxMs += 1000 * samples / rate;
    if (ctx.state !== 'running') {
      if (ctx.state === 'suspended') tryResume();
      st.notRunning++; tot.idle++; nextTime = 0;
      return;
    }
    if (rate !== streamRate) {
      streamRate = rate; ratio = rate / ctx.sampleRate; phase = 0; tail = null;
      if (ratio !== 1) console.warn('[mlaudio] the audio context runs at ' + ctx.sampleRate + ' Hz, the stream at ' + rate + ' Hz: resampling');
      console.log('[mlaudio] rendering ' + ch + ' channels of ' + rate + ' Hz audio, jitter buffer ' + targetSeconds() * 1000 + ' ms');
    }
    var now = ctx.currentTime, target = targetSeconds();
    if (nextTime < now) {   // first frame or stall: prime to the whole jitter buffer
      if (nextTime !== 0) { st.underruns++; tot.underruns++; }
      nextTime = now + target; phase = 0; tail = null;
      st.resyncs++; tot.resyncs++;
    }
    if (nextTime > now + 2 * target) { st.droppedFull++; tot.dropped++; logStats(false); return; }
    var pcm = new Int16Array(data, 0, samples * ch);   // host sends little endian; the TV is little endian too
    var buf = build(pcm, samples, ch);
    if (!buf) { st.droppedEmpty++; tot.empty++; logStats(false); return; }
    var src = ctx.createBufferSource();
    src.buffer = buf;
    src.connect(ctx.destination);
    src.start(nextTime);
    recordHeadroom(nextTime - now);
    st.scheduled++; tot.scheduled++;
    nextTime += buf.duration;
    logStats(false);
  };
})();
