// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A shader layer over `__ps5ReactNative.shader` (ABI v10), without React: ShaderView drives one.

/**
 * A uniform value as the host takes it: a number, or 2 to 4 numbers. Color strings ('#rgb', '#rgba',
 * '#rrggbb', '#rrggbbaa', 'rgb()', 'rgba()') become [r, g, b, a] in 0..1.
 * @param {string} name @param {number | number[] | string} value @returns {number | number[]}
 */
export function uniformValue(name, value) {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (Array.isArray(value) && value.length >= 2 && value.length <= 4 && value.every(Number.isFinite)) return value;
  if (typeof value === 'string') {
    const color = parseColor(value);
    if (color) return color;
  }
  throw new TypeError(`ShaderView uniform ${name}: expected a number, 2 to 4 numbers or a color, got ${JSON.stringify(value)}`);
}

/** @param {string} text @returns {number[] | null} */
function parseColor(text) {
  const s = text.trim().toLowerCase();
  let m = /^#([0-9a-f]{3,8})$/.exec(s);
  if (m && [3, 4, 6, 8].includes(m[1].length)) {
    const hex = m[1].length <= 4 ? [...m[1]].map(c => c + c).join('') : m[1];
    const channels = hex.match(/../g).map(h => parseInt(h, 16) / 255);
    return channels.length === 3 ? [...channels, 1] : channels;
  }
  m = /^rgba?\(([^)]*)\)$/.exec(s);
  if (!m) return null;
  const parts = m[1].split(/[\s,/]+/).filter(Boolean).map(Number);
  if ((parts.length !== 3 && parts.length !== 4) || !parts.every(Number.isFinite)) return null;
  return [parts[0] / 255, parts[1] / 255, parts[2] / 255, parts[3] ?? 1];
}

/** @param {Record<string, number | number[] | string> | undefined} uniforms */
export function convertUniforms(uniforms) {
  const out = {};
  for (const [name, value] of Object.entries(uniforms ?? {})) out[name] = uniformValue(name, value);
  return out;
}

/** The host's shader namespace, or an error that says what is missing. */
export function shaderHost() {
  const api = globalThis.__ps5ReactNative?.shader;
  if (!api) throw new Error('ShaderView needs a PS5 React host with shader layers (ABI v10); run it with npm run dev or a PS5 build');
  return api;
}

/**
 * One native layer: created with its source, then updated with options and its rect as they come.
 * `poll()` once the host has drawn a frame returns the GLSL info log of a failed compile, once.
 */
export class ShaderLayer {
  /** @param {ReturnType<typeof shaderHost>} api @param {string} source */
  constructor(api, source) {
    this.api = api;
    this.id = api.create(source);
    this.reported = false;
  }
  /** @param {{x?: number, y?: number, width?: number, height?: number, opacity?: number,
   *   resolutionScale?: number, uniforms?: Record<string, number | number[]>}} options */
  update(options) {
    if (this.id) this.api.update(this.id, options);
  }
  /** @returns {string | null} */
  poll() {
    if (!this.id || this.reported) return null;
    this.reported = true;
    return this.api.error(this.id);
  }
  destroy() {
    if (this.id) this.api.destroy(this.id);
    this.id = 0;
  }
}
