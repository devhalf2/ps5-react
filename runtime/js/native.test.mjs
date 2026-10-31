// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import test from 'node:test';
import {acquireImage, Archives, BrowserCapture, Daemon, Downloads, FileSystem, Http, Notifications, Power, Sound, System} from './native.js';
import {setFrameBatcher} from './frame.js';

test('native network tasks deliver progress, release listeners and stop polling', async () => {
  let tick;
  let clears = 0;
  let snapshots = [];
  const cancelled = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => { clears++; };
  globalThis.__ps5ReactNative = {network: {
    download: () => 1, request: () => 2,
    cancel: id => cancelled.push(id), poll: () => snapshots, version: () => 'fixture',
  }};
  try {
    const task = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    const states = [];
    const unsubscribe = task.subscribe(progress => states.push(progress.state));
    snapshots = [{id: 1, state: 'downloading', body: 'must not reach progress', written: 12}];
    tick();
    assert.equal(task.snapshot.destination, '/download0/file');
    assert.equal('body' in task.snapshot, false);
    unsubscribe();
    snapshots = [{id: 1, state: 'completed', written: 24}];
    tick();
    assert.equal((await task.done).written, 24);
    assert.deepEqual(states, ['queued', 'downloading']);
    assert.equal(clears, 1);
    task.cancel();
    assert.deepEqual(cancelled, []);
    const aborted = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    aborted.cancel();
    snapshots = [{id: 1, state: 'cancelled'}];
    tick();
    await assert.rejects(aborted.done, error => error.name === 'AbortError' && error.message.includes('/download0/file'));
    assert.deepEqual(cancelled, [1]);
    const response = Http.request('https://example.com/json');
    snapshots = [{id: 2, state: 'completed', status: 404, url: 'https://example.com/final', headers: {'content-type': 'application/json'}, body: '{"value":42}'}];
    tick();
    const result = await response;
    assert.equal(result.ok, false);
    assert.equal(result.status, 404);
    assert.equal(result.url, 'https://example.com/final');
    assert.equal(result.headers['content-type'], 'application/json');
    assert.equal(Object.isFrozen(result.headers), true);
    assert.deepEqual(await result.json(), {value: 42});
    assert.equal(await result.text(), '{"value":42}');
    assert.equal(clears, 3);
  } finally {
    globalThis.setInterval = originalInterval;
    globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});

test('removeAsync resolves once the worker removed the path and rejects with the path and system error', async () => {
  let tick;
  let snapshots = [];
  const started = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps5ReactNative = {
    fs: {removeAsync: path => {
      if (path.includes('..')) throw new Error(`fs.removeAsync ${path}: Path is not allowed`);
      return started.push(path);
    }},
    removals: {poll: () => snapshots},
  };
  try {
    const removed = FileSystem.removeAsync('/download0/game');
    const refused = FileSystem.removeAsync('/download0/locked');
    assert.deepEqual(started, ['/download0/game', '/download0/locked']);
    snapshots = [{id: 1, state: 'removing', error: ''}, {id: 2, state: 'queued', error: ''}];
    tick();
    snapshots = [{id: 1, state: 'completed', error: ''}, {id: 2, state: 'failed', error: 'Permission denied'}];
    tick();
    assert.equal(await removed, undefined);
    await assert.rejects(refused, {message: 'fs.removeAsync /download0/locked: Permission denied'});
    await assert.rejects(FileSystem.removeAsync('/download0/../x'), /Path is not allowed/);
  } finally {
    globalThis.setInterval = originalInterval;
    globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});

test('remote images poll only while loads are pending and release every reference', () => {
  const released = [];
  let finished = [];
  let polls = 0;
  globalThis.__ps5ReactNative = {image: {
    load: (uri, width, height) => (width === 10
      ? {id: 7, state: 'ready', name: '@image:7', width: 10, height: 10, error: ''}
      : {id: 8, state: 'loading', name: '', width: 0, height: 0, error: ''}),
    release: id => released.push(id),
    poll: () => { polls++; const out = finished; finished = []; return out; },
  }};
  const seen = [];
  const releaseReady = acquireImage('https://example.com/a.jpg', 10, 10, 0, false, result => seen.push(result.state));
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 0, 'a cached image needs no polling');
  const releaseLoading = acquireImage('https://example.com/b.jpg', 20, 20, 0, false, result => seen.push(result.state));
  globalThis.__ps5ReactFrame(16);
  finished = [{id: 8, state: 'ready', name: '@image:8', width: 20, height: 20, error: ''}];
  globalThis.__ps5ReactFrame(16);
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 2, 'polling stops once nothing is pending');
  assert.deepEqual(seen, ['ready', 'loading', 'ready']);
  releaseReady();
  releaseLoading();
  assert.deepEqual(released, [7, 8]);
  globalThis.__ps5ReactFrame(16);
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 3, 'a release polls once more so eviction can run');
  delete globalThis.__ps5ReactNative;
});

test('finished image loads arriving together are delivered across frames under a time budget', () => {
  const originalNow = performance.now;
  let clock = 0, polls = 0, nextId = 1, finished = [];
  performance.now = () => clock;
  globalThis.__ps5ReactNative = {image: {
    load: () => ({id: nextId++, state: 'loading', name: '', width: 0, height: 0, error: ''}),
    release: () => {},
    poll: () => { polls++; const out = finished; finished = []; return out; },
  }};
  try {
    let frame = [];
    const frames = [];
    const step = () => { globalThis.__ps5ReactFrame(16); frames.push(frame); frame = []; };
    // Each arrival costs what re-rendering its <Image> would: 1.5 ms, or 10 ms for id 6.
    const releases = new Map();
    for (let i = 1; i <= 9; i++) {
      releases.set(i, acquireImage(`https://example.com/${i}.jpg`, 20, 20, 0, false, result => {
        if (result.state === 'loading') return;
        frame.push(result.state === 'failed' ? `${result.id}:${result.error}` : result.id);
        clock += result.id === 6 ? 10 : 1.5;
      }));
    }
    releases.get(3)();
    finished = [1, 2, 3, 4, 5, 6, 7, 8, 9].map(id => (id === 9
      ? {id, state: 'failed', name: '', width: 0, height: 0, error: 'HTTP 404'}
      : {id, state: 'ready', name: `@image:${id}`, width: 20, height: 20, error: ''}));
    step();
    releases.get(5)();
    while (frames.length < 6) step();
    assert.deepEqual(frames.filter(frame => frame.length), [[1, 2, 4], [6], [7, 8, '9:HTTP 404']],
      'released ids are skipped for free, a slow arrival still lands, failures are delivered too');
    assert.equal(polls, 3, 'polling continues while results are queued and stops once all are delivered');
  } finally {
    performance.now = originalNow;
    delete globalThis.__ps5ReactNative;
  }
});

test('image listener commits are measured individually despite the frame batcher', () => {
  const originalNow = performance.now;
  let finished = [], clock = 0;
  const delivered = [];
  const deferredBatches = [];
  performance.now = () => clock;
  setFrameBatcher(run => deferredBatches.push(run));
  globalThis.__ps5ReactNative = {image: {
    load: () => ({id: 1, state: 'loading', name: '', width: 0, height: 0, error: ''}),
    release: () => {},
    poll: () => { const out = finished; finished = []; return out; },
  }};
  try {
    for (let id = 1; id <= 7; id++) acquireImage(`https://example.com/${id}.jpg`, 20, 20, 0, false, result => {
      if (result.state === 'ready') { delivered.push(id); clock += 3; }
    });
    finished = [{id: 1, state: 'ready', name: '@image:1', width: 20, height: 20, error: ''}];
    const batches = [];
    for (let frame = 0; frame < 4; frame++) {
      globalThis.__ps5ReactFrame(16);
      batches.push(delivered.length);
    }
    assert.deepEqual(batches, [2, 4, 6, 7], 'the next listener waits until the next frame after commits cross 4 ms');
    assert.deepEqual(delivered, [1, 2, 3, 4, 5, 6, 7]);
    assert.equal(deferredBatches.length, 4, 'listeners ran although no batch did; each frame still closes with one');
  } finally {
    setFrameBatcher(run => run());
    performance.now = originalNow;
    delete globalThis.__ps5ReactNative;
  }
});

test('Power.keepAwake passes a boolean to the host', () => {
  const calls = [];
  globalThis.__ps5ReactNative = {power: {keepAwake: enabled => calls.push(enabled)}};
  try {
    Power.keepAwake(true);
    Power.keepAwake(0);
    assert.deepEqual(calls, [true, false]);
  } finally {
    delete globalThis.__ps5ReactNative;
  }
});

test('Sound passes the name, volume and pan to the host and returns whether it started', () => {
  const calls = [];
  globalThis.__ps5ReactNative = {sound: {
    play: (name, volume, pan) => { calls.push(['play', name, volume, pan]); return name === 'tick'; },
    setVolume: volume => calls.push(['setVolume', volume]),
  }};
  try {
    assert.equal(Sound.play('tick'), true);
    assert.equal(Sound.play('page', {pan: -0.4}), false);
    Sound.play('tick', {volume: 0.5});
    Sound.setVolume(0);
    assert.deepEqual(calls, [['play', 'tick', 1, 0], ['play', 'page', 1, -0.4], ['play', 'tick', 0.5, 0],
      ['setVolume', 0]]);
  } finally {
    delete globalThis.__ps5ReactNative;
  }
});

test('archive and download task IDs are isolated while sharing the polling timer', async () => {
  const originalInterval = globalThis.setInterval, originalClear = globalThis.clearInterval;
  let tick, networkSnapshots = [], archiveSnapshots = [];
  const cancelled = [];
  const extractions = [];
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps5ReactNative = {
    network: {download: () => 1, poll: () => networkSnapshots, cancel: id => cancelled.push(`network:${id}`)},
    archives: {extract: (...args) => { extractions.push(args); return 1; }, poll: () => archiveSnapshots, cancel: id => cancelled.push(`archives:${id}`)},
  };
  try {
    const download = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    const archive = Archives.extract({sources: ['/download0/file'], destination: '/download0/out', password: '[DLPSGAME.COM]'});
    assert.deepEqual(extractions, [[['/download0/file'], '/download0/out', 1024**4, '[DLPSGAME.COM]', false, null, []]]);
    assert.equal(Archives.streams, true);
    for (const password of [42, 'bad\0password', 'x'.repeat(1025)])
      assert.throws(() => Archives.extract({sources: ['/download0/file'], destination: '/download0/out', password}), /password/);
    assert.equal(extractions.length, 1);
    archive.cancel(); assert.deepEqual(cancelled, ['archives:1']);
    archiveSnapshots = [{id: 1, state: 'extracting', phase: 'finishing', written: 50, synced: 1, syncTotal: 3}]; tick();
    assert.deepEqual([archive.snapshot.phase, archive.snapshot.synced, archive.snapshot.syncTotal], ['finishing', 1, 3]);
    archiveSnapshots = [{id: 1, state: 'completed', written: 50, artifacts: ['game.ffpfsc']}]; tick();
    assert.deepEqual((await archive.done).artifacts, ['game.ffpfsc']);
    assert.equal(download.snapshot.state, 'queued');
    networkSnapshots = [{id: 1, state: 'completed', written: 100}]; tick();
    assert.equal((await download.done).written, 100);
    // A downloading 7z: its address and headers, as "Name: value" lines, for the engine to fetch its end.
    const streamed = Archives.extract({sources: ['/download0/file'], destination: '/download0/out', stream: true,
      tail: {url: 'https://example.com/file', headers: {Cookie: 'a=b'}}});
    assert.deepEqual(extractions.pop(), [['/download0/file'], '/download0/out', 1024**4, '', true, 'https://example.com/file', ['Cookie: a=b']]);
    assert.equal(Archives.streamsSevenZip, true);
    archiveSnapshots = [{id: 1, state: 'completed', written: 0, artifacts: []}]; tick();
    await streamed.done;
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});


test('browser capture delivers only its result and cancellation rejects as AbortError', async () => {
  const originalInterval = globalThis.setInterval, originalClear = globalThis.clearInterval;
  let tick, snapshots = [], next = 1;
  const calls = [], cancelled = [];
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps5ReactNative = {browser: {
    capture: (...args) => { calls.push(args); return next++; },
    poll: () => { const result = snapshots; snapshots = []; return result; },
    cancel: id => cancelled.push(id),
  }};
  try {
    const capture = BrowserCapture.capture({suffix: '/Game.pkg'});
    assert.deepEqual(calls, [['https://vikingfile.com/d/', '/Game.pkg', 180]]);
    const states = [];
    const off = capture.subscribe(snapshot => states.push(snapshot.state));
    snapshots = [{id: 1, state: 'capturing'}]; tick();
    const url = 'https://vikingfile.com/d/Ab01234567/Game.pkg';
    snapshots = [{id: 1, state: 'completed', url}]; tick();
    assert.equal((await capture.done).url, url);
    assert.deepEqual(states, ['queued', 'capturing', 'completed']); off();
    const second = BrowserCapture.capture({suffix: '/Other.pkg', timeoutSeconds: 30});
    second.cancel(); assert.deepEqual(cancelled, [2]);
    snapshots = [{id: 2, state: 'cancelled'}]; tick();
    await assert.rejects(second.done, {name: 'AbortError'});
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});

test('System delivers state changes on its own timer and stops polling with the last listener', () => {
  let tick = null;
  let clears = 0;
  let queued = [{state: 'sleeping', at: 1, source: 'event'}];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  const originalError = console.error;
  globalThis.setInterval = (callback, ms) => { assert.equal(ms, 500); tick = callback; return 7; };
  globalThis.clearInterval = id => { assert.equal(id, 7); clears++; tick = null; };
  const simulated = [];
  globalThis.__ps5ReactNative = {system: {
    poll: () => { const events = queued; queued = []; return events; },
    runningGame: () => ({titleId: 'PPSA01234'}),
    simulate: (state, titleId) => { simulated.push([state, titleId]); return true; },
  }};
  try {
    assert.throws(() => System.onStateChange(null), /System.onStateChange: expected a function/);
    const first = [];
    const second = [];
    const unsubscribeFirst = System.onStateChange(event => first.push(event));
    const unsubscribeSecond = System.onStateChange(event => { second.push(event.state); throw new Error('listener'); });
    assert.equal(queued.length, 0, 'changes from before the first listener are dropped');
    queued = [{state: 'resumed', at: 2, source: 'gap', pausedMs: 61000},
      {state: 'game-started', at: 3, source: 'game-poll', titleId: 'PPSA01234'}];
    const errors = [];
    console.error = (...args) => errors.push(args[0]);
    tick();
    assert.deepEqual(first.map(event => event.state), ['resumed', 'game-started']);
    assert.equal(first[0].pausedMs, 61000);
    assert.equal(Object.isFrozen(first[1]), true);
    assert.deepEqual(second, ['resumed', 'game-started']);
    assert.equal(errors.length, 2);
    unsubscribeFirst();
    unsubscribeFirst();
    assert.equal(clears, 0);
    unsubscribeSecond();
    assert.equal(clears, 1);
    assert.deepEqual(System.runningGame(), {titleId: 'PPSA01234'});
    assert.equal(System.simulate('game-started', {titleId: 'PPSA05678'}), true);
    System.simulate('sleeping');
    assert.deepEqual(simulated, [['game-started', 'PPSA05678'], ['sleeping', '']]);
  } finally {
    globalThis.setInterval = originalInterval;
    globalThis.clearInterval = originalClear;
    console.error = originalError;
    delete globalThis.__ps5ReactNative;
  }
});

test('Daemon.start resolves ok or the reason, never rejects, and reads the host flags', async () => {
  const originalInterval = globalThis.setInterval, originalClear = globalThis.clearInterval;
  let tick;
  let snapshots = [];
  const starts = [];
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps5ReactNative = {daemon: {supported: true, isDaemon: false, args: null,
    start: (token, stateDir) => { starts.push([token, stateDir]); return starts.length; }, poll: () => snapshots, cancel() {}}};
  try {
    assert.deepEqual([Daemon.supported, Daemon.isDaemon, Daemon.args, Daemon.pid, Daemon.port], [true, false, null, null, 9061]);
    const ready = Daemon.start({token: 'a'.repeat(32), stateDir: '/data/queue'});
    await Promise.resolve();
    snapshots = [{id: 1, state: 'completed', error: ''}];
    tick();
    assert.deepEqual(await ready, {ok: true});
    const refused = Daemon.start({token: 'a'.repeat(32), stateDir: '/data/queue'});
    await Promise.resolve();
    snapshots = [{id: 2, state: 'failed', error: 'pandorad was not ready within 10 s; see daemon.log'}];
    tick();
    assert.deepEqual(await refused, {ok: false, error: 'Daemon.start: pandorad was not ready within 10 s; see daemon.log'});
    assert.deepEqual(starts, [['a'.repeat(32), '/data/queue'], ['a'.repeat(32), '/data/queue']]);
    globalThis.__ps5ReactNative.daemon.start = () => { throw new TypeError('Daemon.start: stateDir must be an absolute path'); };
    assert.deepEqual(await Daemon.start({token: 'x', stateDir: 'relative'}),
      {ok: false, error: 'Daemon.start: stateDir must be an absolute path'});
    globalThis.__ps5ReactNative = {};
    assert.equal(Daemon.supported, false);
    assert.equal((await Daemon.start({token: 'x', stateDir: '/data'})).ok, false);
    assert.throws(() => Daemon.serve(() => ({status: 200})), /only the daemon serves/);
    assert.throws(() => Daemon.exit(0), /only the daemon exits/);
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});

test('Daemon.serve hands each request to the handler and answers what it returns, or 500 when it throws', async () => {
  let callback;
  const answers = [];
  const originalError = console.error;
  console.error = () => {};
  globalThis.__ps5ReactNative = {daemon: {supported: true, isDaemon: true, args: {token: 't', stateDir: '/s', version: '1'}, pid: 4321, port: 19061,
    serve: handler => { callback = handler; }, exit() {}}};
  try {
    assert.throws(() => Daemon.serve('handler'), TypeError);
    Daemon.serve(async request => {
      if (request.path === '/throw') throw new Error('broken');
      if (request.path === '/sync') return {status: 202, body: 'sync'};
      return {status: 200, body: JSON.stringify(request.query)};
    });
    const settle = () => new Promise(resolve => setTimeout(resolve, 0));
    for (const path of ['/ok', '/sync', '/throw'])
      callback({method: 'GET', path, query: {a: '1'}, headers: {}, body: ''}, response => answers.push([path, response]));
    await settle();
    assert.deepEqual(answers.map(([path, response]) => [path, response.status]), [['/ok', 200], ['/sync', 202], ['/throw', 500]]);
    assert.equal(answers[0][1].body, '{"a":"1"}');
    assert.match(answers[2][1].body, /broken/);
    assert.deepEqual(Daemon.args, {token: 't', stateDir: '/s', version: '1'});
    assert.equal(Daemon.pid, 4321);
    assert.equal(Daemon.port, 19061);
    assert.equal(Daemon.isDaemon, true);
  } finally {
    console.error = originalError;
    delete globalThis.__ps5ReactNative;
  }
});

test('Notifications.send passes a rich toast to the host with defaults and checks id and title', () => {
  const sent = [];
  globalThis.__ps5ReactNative = {notifications: {send: (...args) => { sent.push(args); return true; }},
    notify: () => assert.fail('show is a separate call')};
  try {
    assert.equal(Notifications.send({id: 'job-1', title: 'Downloading', body: 'Astro Bot', icon: '/download0/icons/a.png'}), true);
    Notifications.send({id: 'job-1', title: 'Installed', kind: 'done'});
    assert.deepEqual(sent, [['job-1', 'Downloading', 'Astro Bot', '/download0/icons/a.png', 'info'],
      ['job-1', 'Installed', '', '', 'done']]);
    assert.throws(() => Notifications.send({title: 'x'}), /Notifications.send: id must be a non-empty string/);
    assert.throws(() => Notifications.send({id: 'job-2'}), /Notifications.send job-2: title must be a string/);
  } finally {
    delete globalThis.__ps5ReactNative;
  }
});
