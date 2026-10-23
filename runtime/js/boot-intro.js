// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// BootIntro: the PS5 boot sequence (logo, bokeh, a burst of dust, a beam of light) as a reusable
// entrance, the scene drawn by one ShaderView; see docs/SHADERS.md.
import {createElement, useEffect, useRef, useState} from 'react';
import {View} from 'embedded-react';
import {ShaderView} from './shader.js';
import {motion} from './motion/index.js';
import {subscribeInput} from './input.js';
import {BOOT_INTRO_STAGES, BOOT_INTRO_TIMING, bootIntroSchedule} from './boot-intro-timeline.js';

export {BOOT_INTRO_STAGES, BOOT_INTRO_TIMING};

// Scene time of the shader's own timeline origin (the burst reference): t = scene - 3.0.
const SCENE_OFFSET = 3.0;

export const bootIntroShader = `#version 410 core
uniform float uTime;
uniform vec2 uResolution;
uniform float uSpeed;
uniform float uFadeOut;
uniform float uStart;
uniform float uBokeh;
out vec4 fragColor;

float hash12(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
vec2 hash22(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.xx + p3.yz) * p3.zy);
}
float disc(vec2 q, vec2 c, float r, float soft) { return 1.0 - smoothstep(r - soft, r + soft, length(q - c)); }
float segment(vec2 p, vec2 a, vec2 b) {
  vec2 ab = b - a;
  float h = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-9), 0.0, 1.0);
  return length(p - a - ab * h);
}

// One depth layer of dust: a jittered grid, one particle in some cells, the 3x3 neighbourhood searched.
// Lengths are in screen heights. The field spreads out from center by spread (the burst) and moves by
// drift; small particles smear over blur seconds of their motion (spreadRate: d(spread)/dt, velocity: the
// drift's). gather (0..1) adds particles where the burst gathers, right of center and above the floor.
vec3 dust(vec2 q, float cell, vec2 drift, vec2 velocity, float rad, float soft, float density, float seed,
          float spread, float spreadRate, vec2 center, float bokeh, float px, float gather, float blur) {
  float A = uResolution.x / uResolution.y;
  vec2 g = center + (q - center) / spread - drift;
  vec2 id0 = floor(g / cell);
  vec3 acc = vec3(0.0);
  for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++) {
      vec2 id = id0 + vec2(i, j);
      vec2 pos = (id + 0.1 + 0.8 * hash22(id * 1.31 + seed)) * cell;
      vec2 screen = center + (pos + drift - center) * spread;
      float local = density * (1.0 + gather * 3.0 * smoothstep(0.5 * A, 0.85 * A, screen.x)
                                        * (1.0 - smoothstep(0.7, 0.95, screen.y)));
      float h = hash12(id + seed);
      if (h > local) continue;
      float k = h / local;
      float r = rad * mix(0.45, 1.6, k * k);
      vec2 v = (pos + drift - center) * spreadRate + velocity * spread;
      float d = bokeh > 0.5 ? length(q - screen) : segment(q, screen, screen - v * blur);
      float s = max(soft, px * 0.7);
      float rr = max(r, px * 0.6);
      float cov = 1.0 - smoothstep(rr - s, rr + s, d);
      // A smear spreads the same light over its length.
      if (bokeh < 0.5) cov *= min(1.0, (r * r) / (rr * rr)) * rr / (rr + 0.5 * length(v) * blur);
      float twinkle = 0.85 + 0.15 * sin(uTime * (2.0 + 5.0 * fract(k * 7.1)) + k * 60.0);
      vec3 col = mix(vec3(0.86, 0.60, 0.36), vec3(0.98, 0.86, 0.72), fract(k * 7.7) * fract(k * 7.7));
      acc += col * cov * twinkle * (0.12 + 1.15 * pow(fract(k * 3.9), 3.0));
    }
  return acc;
}

void main() {
  float A = uResolution.x / uResolution.y;
  vec2 q = vec2(gl_FragCoord.x, uResolution.y - gl_FragCoord.y) / uResolution.y;
  float px = 1.0 / uResolution.y;
  // t: reference seconds, 0 three seconds into the scene (the teal discs come at 0.5, the burst at 0.8).
  // uStart: the scene time the layer opens at (past the bokeh when it is skipped).
  float t = uStart + uTime * uSpeed - ${SCENE_OFFSET.toFixed(1)};
  vec3 col = vec3(0.0);

  // Navy bokeh: a huge disc that comes in from the right and grows, a second behind it, then teal discs.
  float grow = 1.0 - pow(1.0 - clamp((t + 3.0) / 2.2, 0.0, 1.0), 2.2);
  float eA = uBokeh * smoothstep(-3.0, -1.6, t) * (0.75 + 0.25 * smoothstep(-0.9, 0.6, t)) * (1.0 - smoothstep(0.9, 1.08, t));
  col += eA * vec3(0.038, 0.100, 0.165) * disc(q, vec2(mix(1.05, 0.66, grow) * A, 0.52), mix(0.55, 0.80, grow) + 0.05 * max(t, 0.0), 0.08);
  col += eA * vec3(0.012, 0.025, 0.040) * disc(q, vec2(mix(1.15, 0.86, grow) * A, 0.42), mix(0.40, 0.56, grow), 0.07);
  float eT = uBokeh * smoothstep(0.5, 0.72, t) * (1.0 - smoothstep(0.8, 1.0, t));
  for (int i = 0; i < 11; i++) {
    vec2 c = vec2((0.30 + 0.72 * hash12(vec2(i, 3.0))) * A, 0.05 + 0.9 * hash12(vec2(i, 7.0)));
    c += vec2(0.03, -0.02) * t;
    float r = 0.10 + 0.08 * hash12(vec2(i, 11.0));
    float b = 0.55 + 0.9 * hash12(vec2(i, 13.0));
    col += eT * b * vec3(0.045, 0.072, 0.085) * disc(q, c, r, 0.01) * (0.85 + 0.3 * smoothstep(r * 0.6, r, length(q - c)));
  }

  // The room: a beam from the top left, a darker right side, haze over the floor.
  float x = 1.0 - pow(1.0 - clamp((t - 1.15) / 1.6, 0.0, 1.0), 1.6);
  vec3 room = mix(vec3(0.150, 0.138, 0.165), vec3(0.052, 0.050, 0.098), smoothstep(0.25 * A, 1.0 * A, q.x));
  room *= 1.0 - 0.35 * smoothstep(0.2, 1.0, (q.x / A) * (1.0 - q.y));
  vec2 v = q - vec2(0.17 * A, -0.06);
  float along = length(v);
  float angle = atan(v.x, v.y) - 0.62;
  float beam = 1.3 * exp(-pow(angle / 0.48, 2.0)) * exp(-along * 2.0) * smoothstep(0.0, 0.12, along);
  float glow = exp(-length((q - vec2(0.17 * A, -0.03)) * vec2(1.0, 1.4)) * 7.0);
  vec3 beamColor = vec3(0.44, 0.36, 0.34);
  float haze = exp(-pow((q.y - 0.90) / 0.065, 2.0)) * (0.55 + 0.45 * (1.0 - smoothstep(0.1 * A, 0.9 * A, q.x)));
  vec3 hazeColor = mix(vec3(0.075, 0.075, 0.085), vec3(0.075, 0.095, 0.115), 1.0 - smoothstep(0.0, 0.6 * A, q.x));
  vec3 burstBg = vec3(0.012, 0.022, 0.050) * (1.0 - smoothstep(0.85, 1.35, t)) * smoothstep(0.75, 0.95, t);
  col += burstBg + x * (room + beamColor * beam * 1.35 + beamColor * glow * 3.0 + hazeColor * haze);

  // Dust: a burst that spreads from the right of center, then a drift lit by the beam.
  float eB = smoothstep(0.78, 0.95, t);
  float settle = 1.0 - 0.7 * smoothstep(1.3, 2.9, t);
  float burst = max(t - 0.8, 0.0);
  float spread = 1.0 + 0.6 * (1.0 - exp(-2.6 * burst));
  float spreadRate = t > 0.8 ? 0.6 * 2.6 * exp(-2.6 * burst) : 0.0;
  vec2 center = vec2(0.66 * A, 0.42);
  vec2 flow = vec2(0.20, -0.09) * smoothstep(0.9, 1.6, t);
  vec2 drift = vec2(0.20, -0.09) * max(t - 1.25, 0.0);
  float lit = 1.1 + 3.0 * beam + 1.0 * glow;
  float gather = 1.0 - smoothstep(1.3, 2.0, t);
  float cluster = mix(1.0, 0.6 + 0.8 * smoothstep(0.45 * A, 0.8 * A, q.x), gather);
  const float shutter = 1.0 / 60.0;
  vec3 d = vec3(0.0);
  d += 1.4 * dust(q, 0.014, drift * 0.5, flow * 0.5, 0.0009, 0.0004, 0.07, 1.0, spread, spreadRate, center, 0.0, px, 0.3 * gather, shutter);
  d += 2.0 * cluster * dust(q, 0.026, drift, flow, 0.0019, 0.0007, 0.10, 2.0, spread, spreadRate, center, 0.0, px, gather, shutter);
  d += 1.6 * cluster * dust(q, 0.050, drift * 1.8, flow * 1.8, 0.0034, 0.0020, 0.09, 3.0, spread, spreadRate, center, 0.0, px, gather, shutter);
  float bokehGain = 0.2 + 0.8 * (1.0 - smoothstep(1.2, 1.9, t));
  d += 0.75 * bokehGain * dust(q, 0.09, drift * 2.6, flow * 2.6, mix(0.010, 0.016, gather), 0.0045, 0.22, 4.0, spread, spreadRate, center, 1.0, px, gather, 0.0);
  col += eB * settle * mix(0.75, lit, smoothstep(1.1, 1.8, t)) * d;

  // Vignette, the optional fade to black at the end, and dither against banding.
  vec2 e = (q - vec2(0.5 * A, 0.5)) / vec2(A, 1.0);
  col *= 1.0 - 0.3 * smoothstep(0.4, 0.9, length(e) * 1.3);
  col *= 1.0 - uFadeOut * smoothstep(2.85, 3.3, t);
  col += (hash12(gl_FragCoord.xy + fract(uTime) * 91.0) - 0.5) / 255.0;
  fragColor = vec4(col, 1.0);
}
`;

const fill = {position: 'absolute', left: 0, top: 0, right: 0, bottom: 0};

// The buttons that skip the intro when `skippable`: Cross, Circle, Options.
const SKIP_ACTIONS = new Set(['confirm', 'back', 'options']);
// A skip crosses over in this long, in seconds (not scaled by speed).
const SKIP_REVEAL = 0.25;

/**
 * The PS5 boot sequence as an entrance: `logo` fades in and out on black, then the scene plays (navy
 * bokeh, a burst of warm dust, a beam of light with drifting particles) and `children` cross over it, as
 * the home screen does on the console. Without children the scene fades to black. `onDone` runs once the
 * scene is gone. Real time by default (`speed` 1, about 9.4 s with `logoHold` 1.5); `speed` 2 halves it.
 * `skip` leaves stages out (BOOT_INTRO_STAGES: 'logo' or 'bokeh'); without the bokeh the logo hands over
 * to the dust burst. `skippable` lets Cross, Circle or Options cut to `children` at once. `children`
 * mount when they start crossing over. The scene runs on the GPU; only the logo and the crossover touch
 * the CPU.
 * `logoIn` 0 shows the logo from the first frame (to continue a system splash that showed it).
 * @param {{logo?: any, children?: any, onDone?: () => void, logoIn?: number, logoHold?: number, speed?: number,
 *   skip?: string[], skippable?: boolean, resolutionScale?: number, style?: any}} props
 */
export function BootIntro({logo, children, onDone, logoIn = BOOT_INTRO_TIMING.logoIn, logoHold = BOOT_INTRO_TIMING.logoHold, speed = 1,
  skip = [], skippable = false, resolutionScale = 1, style}) {
  const T = BOOT_INTRO_TIMING, s = 1 / speed;
  const hasLogo = logo != null && !skip.includes('logo');
  const at = bootIntroSchedule({logo: hasLogo, logoIn, logoHold, speed, skip});
  const [phase, setPhase] = useState(hasLogo ? 'logo' : 'scene');
  const [reveal, setReveal] = useState(T.reveal * s);
  const done = useRef(onDone);
  done.current = onDone;
  const timers = useRef([]);
  const after = (seconds, next) => timers.current.push(setTimeout(next, seconds * 1000));
  const clear = () => { timers.current.forEach(clearTimeout); timers.current = []; };
  const finish = () => { setPhase('done'); done.current?.(); };
  useEffect(() => {
    if (hasLogo) {
      after(at.logoOut, () => setPhase('logo-out'));
      after(at.lead, () => setPhase('lead'));
      after(at.scene, () => setPhase('scene'));
    }
    after(at.reveal, () => setPhase('reveal'));
    after(at.done, finish);
    return clear;
  }, []);
  const phaseNow = useRef(phase);
  phaseNow.current = phase;
  useEffect(() => {
    if (!skippable) return undefined;
    return subscribeInput(action => {
      if (!SKIP_ACTIONS.has(action) || phaseNow.current === 'reveal' || phaseNow.current === 'done') return;
      clear();
      setReveal(SKIP_REVEAL);
      setPhase('reveal');
      after(SKIP_REVEAL, finish);
    });
  }, [skippable]);

  const showScene = phase === 'scene' || phase === 'reveal';
  const revealed = phase === 'reveal' || phase === 'done';
  return createElement(View, {style: [fill, {backgroundColor: '#000000'}, style]},
    phase === 'logo' || phase === 'logo-out' ? createElement(motion.View, {
      key: 'logo', style: [fill, {alignItems: 'center', justifyContent: 'center'}], initial: logoIn > 0 ? {opacity: 0} : false,
      animate: phase === 'logo' ? {opacity: 1, transition: {duration: logoIn * s, ease: 'easeOut'}}
        : {opacity: 0, transition: {duration: T.logoOut * s, ease: 'easeIn'}},
    }, logo) : null,
    showScene ? createElement(ShaderView, {
      key: 'scene', source: bootIntroShader, resolutionScale, style: fill,
      uniforms: {uSpeed: speed, uFadeOut: children == null ? 1 : 0, uStart: at.start, uBokeh: skip.includes('bokeh') ? 0 : 1},
    }) : null,
    revealed && children != null ? createElement(motion.View, {
      key: 'next', style: fill, initial: {opacity: 0},
      animate: {opacity: 1, transition: {duration: reveal, ease: 'easeInOut'}},
    }, children) : null);
}
