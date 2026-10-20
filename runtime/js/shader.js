// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// ShaderView: a GLSL fragment shader drawn on the GPU inside a laid-out rect; see docs/SHADERS.md.
import {createElement, useLayoutEffect, useRef} from 'react';
import {View} from 'embedded-react';
import {onFrame} from './frame.js';
import {convertUniforms, ShaderLayer, shaderHost} from './shader-layer.js';

/**
 * GLSL fragment shader drawn on the GPU in this View's rect. The rasterizer leaves the rect transparent (a
 * cutout) and the presenter draws the shader under the UI, so children and anything painted later show above
 * it. The engine sets uTime (seconds since the layer was created), uResolution (the layer's pixels),
 * uRect (x, y, width, height in framebuffer pixels) and uOpacity every frame; animation needs no JavaScript.
 * Changing uniforms, opacity or resolutionScale does not recompile; changing `source` does.
 * @param {{source: string, uniforms?: Record<string, number | number[] | string>, opacity?: number,
 *   resolutionScale?: number, style?: any, children?: any, onError?: (log: string) => void,
 *   onLayout?: (event: object) => void}} props
 */
export function ShaderView({source, uniforms, opacity = 1, resolutionScale = 1, style, children, onError, ...rest}) {
  const state = useRef({layer: null, rect: null});
  const options = {opacity, resolutionScale, uniforms: convertUniforms(uniforms)};
  const latest = useRef(options);
  latest.current = options;
  const errorHandler = useRef(onError);
  errorHandler.current = onError;

  useLayoutEffect(() => {
    const layer = new ShaderLayer(shaderHost(), source);
    state.current.layer = layer;
    layer.update({...latest.current, ...state.current.rect});
    // The host compiles when it first draws the layer; report a failure once, a frame later.
    let frames = 0;
    const stop = onFrame(() => {
      if (++frames < 2) return;
      stop();
      const log = layer.poll();
      if (!log) return;
      if (errorHandler.current) errorHandler.current(log);
      else console.error(`ShaderView: ${log}`);
    });
    return () => {
      stop();
      state.current.layer = null;
      layer.destroy();
    };
  }, [source]);

  useLayoutEffect(() => { state.current.layer?.update(options); });

  const onLayout = event => {
    const {x, y, width, height} = event.layout;
    state.current.rect = {x, y, width, height};
    state.current.layer?.update(state.current.rect);
    rest.onLayout?.(event);
  };
  return createElement(View, {...rest, style: [style, {cutout: true}], onLayout}, children);
}
