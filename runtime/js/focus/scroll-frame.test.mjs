// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {createFrame} from './scroll-frame.js';

/** A 1280×720 vertical ScrollView over a stand-in NativeUI.scrollTo that clamps to `max`. */
function setup(maxY) {
  const engine = {x: 0, y: 0, maxY};
  globalThis.screen = {width: 1280, height: 720};
  globalThis.NativeUI = {scrollTo(handle, x, y) {
    if (!Number.isNaN(x)) engine.x = x;
    if (!Number.isNaN(y)) engine.y = Math.min(Math.max(y, 0), engine.maxY);
    return [engine.x, engine.y, 0, engine.maxY];
  }};
  engine.deferred = 0;
  globalThis.__ps5ReactNative = {image: {defer: ms => { engine.deferred = ms; }}};
  const targets = [];
  const frame = createFrame(null);
  frame.props = {onScrollTarget: target => targets.push(target)};
  frame.ref(1);
  frame.onLayout({layout: {x: 0, y: 0, width: 1280, height: 720}});
  const settle = () => {
    for (let i = 0; i < 200 && frame.timer !== null; i++) globalThis.__ps5ReactFrame(1000 / 60);
  };
  return {engine, frame, targets, settle};
}

test('onScrollTarget reports each new focus-scroll target once, when the scroll starts', () => {
  const {engine, frame, targets, settle} = setup(5000);
  const card = {x: 0, y: 1000, width: 100, height: 100};
  frame.reveal(card, null);
  assert.deepEqual(targets, [{x: 0, y: 412}], 'the card ends a 32 px margin above the bottom');
  frame.reveal(card, null);
  assert.equal(targets.length, 1, 'an unchanged target is not reported again');
  settle();
  assert.equal(engine.y, 412);
  assert.equal(frame.y, 412);
  assert.equal(targets.length, 1, 'arriving is not a new target');
  assert.equal(engine.deferred, 150, 'image fetches wait while the view moves');
});

test('onScrollTarget reports where the scroll settled when shrinking content clamps it', () => {
  const {engine, frame, targets, settle} = setup(5000);
  frame.reveal({x: 0, y: 1000, width: 100, height: 100}, null);
  engine.maxY = 300;
  settle();
  assert.deepEqual(targets, [{x: 0, y: 412}, {x: 0, y: 300}]);
  assert.equal(frame.y, 300);
});

test('a 120 Hz display scrolls at the 60 Hz speed, through the positions between its steps', () => {
  const run = (hz) => {
    const {engine, frame} = setup(5000);
    frame.reveal({x: 0, y: 3000, width: 100, height: 100}, null);
    const positions = [];
    for (let i = 0; i < 400 && frame.timer !== null; i++) {
      globalThis.__ps5ReactFrame(1000 / hz);
      positions.push(engine.y);
    }
    return positions;
  };
  const at60 = run(60), at120 = run(120);
  assert.equal(at120.at(-1), at60.at(-1));
  assert.ok(Math.abs(at120.length - 2 * at60.length) <= 1, `${at120.length} frames at 120 Hz, ${at60.length} at 60`);
  at60.slice(0, -1).forEach((y, i) => assert.equal(at120[2 * i + 1], y, `tick ${i + 1} lands where 60 Hz put it`));
  const between = at120.filter((y, i) => i % 2 === 0 && y !== (at120[i - 1] ?? 0) && y !== at120[i + 1]);
  assert.ok(between.length > at60.length / 2, 'the frames between ticks show new positions');
});

test('while it animates, position is where the engine shows the view (VirtualList reads it)', () => {
  const {engine, frame} = setup(5000);
  frame.reveal({x: 0, y: 3000, width: 100, height: 100}, null);
  assert.deepEqual(frame.position, [engine.x, engine.y], 'a far scroll starts where its jump put the view');
  for (let i = 0; i < 5; i++) {
    globalThis.__ps5ReactFrame(1000 / 120);
    assert.deepEqual(frame.position, [engine.x, engine.y]);
  }
  assert.ok(frame.position[1] > 0 && frame.timer !== null);
  frame.shift(40);
  assert.equal(frame.position[1], engine.y, 'a shift moves it with the content');
});

test('revealInsets keeps its own room at each edge; left out, 32 px', () => {
  const {frame, targets, settle} = setup(5000);
  frame.props = {...frame.props, revealInsets: {bottom: 64}};
  frame.reveal({x: 0, y: 1000, width: 100, height: 100}, null);
  assert.deepEqual(targets, [{x: 0, y: 444}], 'the card ends 64 px above the bottom');
  settle();
  frame.reveal({x: 0, y: 300, width: 100, height: 100}, null);
  assert.deepEqual(targets[1], {x: 0, y: 268}, 'the top keeps the default 32 px');
  frame.props = {...frame.props, revealInsets: {top: 100, bottom: 64}};
  frame.reveal({x: 0, y: 2000, width: 100, height: 100}, {x: 0, y: 1900, width: 1280, height: 300});
  assert.deepEqual(targets[2], {x: 0, y: 1800}, 'an anchor aligns past the top inset');
});

test('revealSnap lands on the scroll anchors and reveals the anchor row whole', () => {
  const {frame, targets} = setup(5000);
  frame.onLayout({layout: {x: 0, y: 0, width: 1280, height: 392}});
  frame.props = {...frame.props, revealInsets: {top: 4, bottom: 4}, revealSnap: true};
  const rows = Array.from({length: 20}, (_, i) => ({rect: {x: 0, y: 4 + i * 48, width: 352, height: 48}}));
  for (const row of rows) frame.anchors.add(row);
  frame.reveal({x: 0, y: 388, width: 352, height: 48}, rows[8].rect);
  assert.deepEqual(targets, [{x: 0, y: 48}]);
  // A 20 px button in the 48 px row: the row decides, not the button (which needs only 74).
  frame.reveal({x: 0, y: 450, width: 100, height: 20}, rows[9].rect);
  assert.deepEqual(targets[1], {x: 0, y: 96});
  frame.anchors.delete(rows[2]);
  frame.reveal({x: 0, y: 52, width: 352, height: 48}, rows[1].rect);
  assert.deepEqual(targets[2], {x: 0, y: 48});
});

test('scrollMotion ease-out moves fastest on the first frame and is on the target after 12 frames (200 ms); nested views inherit it', () => {
  const {engine, frame} = setup(5000);
  frame.props = {...frame.props, scrollMotion: 'ease-out'};
  frame.reveal({x: 0, y: 1000, width: 100, height: 100}, null);
  const offsets = [];
  for (let i = 0; i < 20 && frame.timer !== null; i++) {
    globalThis.__ps5ReactFrame(1000 / 60);
    offsets.push(engine.y);
  }
  assert.equal(offsets.length, 12, offsets.join());
  assert.equal(offsets.at(-1), 412);
  assert.ok(offsets[1] / 412 > 0.44 && offsets[3] / 412 > 0.72 && offsets[9] / 412 > 0.99, offsets.join());
  const steps = offsets.map((y, i) => y - (offsets[i - 1] ?? 0));
  assert.ok(steps.every((step, i) => step > 0 && (i === 0 || step <= steps[i - 1])), steps.join());
  assert.equal(createFrame(frame).motion(), 'ease-out');
  assert.equal(createFrame(null).motion(), 'steady');
});

test('scrollMotion ease-out keeps its timing at 120 Hz, and a new target eases on from where it is', () => {
  const {engine, frame} = setup(5000);
  frame.props = {...frame.props, scrollMotion: 'ease-out'};
  frame.reveal({x: 0, y: 1000, width: 100, height: 100}, null);
  const at = [];
  for (let i = 0; i < 4; i++) {
    globalThis.__ps5ReactFrame(1000 / 120);
    at.push(engine.y);
  }
  // 33 ms in: as two 60 Hz frames.
  assert.equal(at[3], Math.round(412 * (1 - (1 - 5 / 36) ** 4)));
  frame.reveal({x: 0, y: 1400, width: 100, height: 100}, null);
  for (let i = 0; i < 60 && frame.timer !== null; i++) {
    globalThis.__ps5ReactFrame(1000 / 120);
    at.push(engine.y);
  }
  assert.ok(at.every((y, i) => i === 0 || y >= at[i - 1]), at.join());
  assert.equal(at.at(-1), 812);
  assert.ok(at.length <= 4 + 29, 'the new target settles within 240 ms of coming');
});

test('scrollMotion ease-out settles where shrinking content clamps it', () => {
  const {engine, frame, targets} = setup(5000);
  frame.props = {...frame.props, scrollMotion: 'ease-out'};
  frame.reveal({x: 0, y: 1000, width: 100, height: 100}, null);
  engine.maxY = 200;
  for (let i = 0; i < 20 && frame.timer !== null; i++) globalThis.__ps5ReactFrame(1000 / 60);
  assert.equal(frame.timer, null);
  assert.equal(engine.y, 200);
  assert.deepEqual(targets.at(-1), {x: 0, y: 200});
});

test('scrolling() is true from a reveal until the view rests, in the view and the views inside it', () => {
  const {frame, settle} = setup(5000);
  const inner = createFrame(frame);
  const seen = [];
  frame.listeners.add(() => seen.push(inner.scrolling()));
  assert.equal(frame.scrolling(), false);
  frame.reveal({x: 0, y: 1000, width: 100, height: 100}, null);
  assert.equal(frame.scrolling(), true);
  assert.equal(inner.scrolling(), true, 'a view inside one that scrolls is scrolling too');
  settle();
  assert.equal(frame.scrolling(), false);
  assert.deepEqual(seen, [true, false], 'listeners hear the start and the stop');
});

test('a focus scroll farther than two viewports jumps to one viewport short of the target, then moves the rest', () => {
  for (const motion of ['steady', 'ease-out']) {
    const {engine, frame, targets, settle} = setup(20000);
    frame.props = {...frame.props, scrollMotion: motion};
    engine.y = 12000;
    frame.onScroll({scrollX: 0, scrollY: 12000});
    frame.reveal({x: 0, y: 0, width: 100, height: 100}, null);
    assert.equal(engine.y, 720, `${motion}: one viewport below the top before any frame`);
    assert.deepEqual(targets, [{x: 0, y: 0}]);
    const offsets = [];
    for (let i = 0; i < 200 && frame.timer !== null; i++) {
      globalThis.__ps5ReactFrame(1000 / 60);
      offsets.push(engine.y);
    }
    assert.equal(engine.y, 0, motion);
    assert.ok(offsets.every((y, i) => y < (offsets[i - 1] ?? 720)), `${motion}: only up from there: ${offsets.join()}`);
    assert.ok(offsets.length <= 30, `${motion}: a viewport's worth of frames, not 12000 px of them`);
    // Down, the jump lands a viewport above the target.
    frame.reveal({x: 0, y: 9000, width: 100, height: 100}, null);
    assert.equal(engine.y, 8412 - 720, motion);
    settle();
    assert.equal(engine.y, 8412, motion);
  }
});

test('a focus scroll within two viewports moves all the way, and so does one interrupting a scroll', () => {
  const {engine, frame, settle} = setup(20000);
  frame.reveal({x: 0, y: 1400, width: 100, height: 100}, null);
  globalThis.__ps5ReactFrame(1000 / 60);
  assert.ok(engine.y > 0 && engine.y < 100, 'no jump for 812 px');
  frame.reveal({x: 0, y: 2000, width: 100, height: 100}, null);
  globalThis.__ps5ReactFrame(1000 / 60);
  assert.ok(engine.y < 200, 'a new near target carries on from where the view is');
  settle();
  assert.equal(engine.y, 1412);
});

test('a shift moves only the offsets on the content that moved: a scroll to a header above a list stays on it', () => {
  const {engine, frame, settle} = setup(20000);
  engine.y = 12000;
  frame.onScroll({scrollX: 0, scrollY: 12000});
  frame.reveal({x: 0, y: 0, width: 100, height: 100}, null);
  globalThis.__ps5ReactFrame(1000 / 60);
  const view = engine.y;
  assert.ok(view > 0 && view < 720, view);
  // A VirtualList below a 1000 px header re-centres its rows 6000 px lower while the view is above it.
  frame.shift(-6000, 1000);
  assert.equal(engine.y, view, 'the view above the list stays');
  assert.equal(frame.target[1], 0, 'and so does the target, on the header');
  assert.equal(frame.y, 0);
  settle();
  assert.equal(engine.y, 0, 'the scroll reaches the top');
  // With the view inside the list, the view and its target move with the rows.
  frame.reveal({x: 0, y: 5000, width: 100, height: 100}, null);
  globalThis.__ps5ReactFrame(1000 / 60);
  const inside = engine.y;
  frame.shift(-600, 1000);
  assert.equal(engine.y, inside + 600);
  assert.equal(frame.target[1], 4412 + 600);
  settle();
  assert.equal(engine.y, 5012);
  assert.equal(frame.y, 5012);
});
