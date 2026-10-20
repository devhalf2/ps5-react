// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import test from 'node:test';
import {convertUniforms, ShaderLayer, shaderHost, uniformValue} from './shader-layer.js';
import {BOOT_INTRO_TIMING, bootIntroSchedule} from './boot-intro-timeline.js';

test('uniform values: numbers, vectors and colors; anything else names the uniform', () => {
  assert.equal(uniformValue('uSpeed', 0.5), 0.5);
  assert.deepEqual(uniformValue('uDir', [1, 0]), [1, 0]);
  assert.deepEqual(uniformValue('uColor', '#ff8000'), [1, 128 / 255, 0, 1]);
  assert.deepEqual(uniformValue('uColor', '#f008'), [1, 0, 0, 136 / 255]);
  assert.deepEqual(uniformValue('uColor', 'rgba(255, 0, 51, 0.5)'), [1, 0, 0.2, 0.5]);
  for (const bad of [NaN, [1], [1, 2, 3, 4, 5], [1, NaN], 'red', '#12345', {}, null]) {
    assert.throws(() => uniformValue('uBad', bad), /ShaderView uniform uBad/);
  }
  assert.deepEqual(convertUniforms({a: 1, b: '#000'}), {a: 1, b: [0, 0, 0, 1]});
  assert.deepEqual(convertUniforms(undefined), {});
});

test('a shader layer creates once, updates without recreating, reports a compile error once, destroys', () => {
  const calls = [];
  const api = {
    create: source => { calls.push(['create', source]); return 7; },
    update: (id, options) => calls.push(['update', id, options]),
    error: id => { calls.push(['error', id]); return 'ERROR: 0:1: bad'; },
    destroy: id => calls.push(['destroy', id]),
  };
  const layer = new ShaderLayer(api, 'void main() {}');
  layer.update({x: 0, y: 0, width: 10, height: 10});
  layer.update({uniforms: {uTint: [1, 0, 0]}, opacity: 0.5});
  assert.equal(layer.poll(), 'ERROR: 0:1: bad');
  assert.equal(layer.poll(), null);
  layer.destroy();
  layer.destroy();
  layer.update({opacity: 1});
  assert.deepEqual(calls.map(call => call[0]), ['create', 'update', 'update', 'error', 'destroy']);
  assert.deepEqual(calls[2][2], {uniforms: {uTint: [1, 0, 0]}, opacity: 0.5});
});

test('the shader host is required and named', () => {
  const saved = globalThis.__ps5ReactNative;
  globalThis.__ps5ReactNative = {};
  try {
    assert.throws(() => shaderHost(), /ABI v10/);
  } finally {
    globalThis.__ps5ReactNative = saved;
  }
});

test('boot intro schedule: the reference timeline, scaled by speed, without a logo from the scene', () => {
  const T = BOOT_INTRO_TIMING;
  const full = bootIntroSchedule({logo: true});
  assert.equal(full.logoOut, T.logoIn + T.logoHold);
  assert.equal(full.scene, T.logoIn + T.logoHold + T.logoOut + T.sceneLead);
  assert.ok(Math.abs(full.done - (full.scene + T.scene + T.reveal)) < 1e-9);
  const fast = bootIntroSchedule({logo: true, speed: 2});
  assert.ok(Math.abs(fast.done - full.done / 2) < 1e-9);
  const bare = bootIntroSchedule({logo: false});
  assert.equal(bare.scene, 0);
  assert.ok(Math.abs(bare.done - (T.scene + T.reveal)) < 1e-9);
  assert.throws(() => bootIntroSchedule({logo: true, speed: 0}), /BootIntro/);
});
