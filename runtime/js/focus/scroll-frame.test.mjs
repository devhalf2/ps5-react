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
