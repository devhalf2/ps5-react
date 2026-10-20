# Shaders

The UI is rasterized on the CPU ([ARCHITECTURE.md](ARCHITECTURE.md)), and a
full-screen animated effect is the most expensive thing it can draw: every frame
repaints every pixel. `ShaderView` draws a GLSL fragment shader on the GPU instead,
inside its laid-out rectangle, composited in z-order with the UI. Animation runs
in the shader from `uTime`; no JavaScript runs per frame and the CPU repaints
nothing while it plays.

```jsx
import {ShaderView, Text} from '@ps5-react/core';

const glow = `
uniform float uTime;
uniform vec2 uResolution;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
  vec2 uv = gl_FragCoord.xy / uResolution;
  float pulse = 0.5 + 0.5 * sin(uTime * 2.0 - uv.x * 6.0);
  fragColor = vec4(uColor.rgb * pulse, 1.0);
}`;

<ShaderView source={glow} uniforms={{uColor: '#ffb070'}} className="w-[600px] h-[300px] items-center justify-center">
  <Text className="text-white">Drawn above the shader</Text>
</ShaderView>
```

[`apps/shader-lab`](../apps/shader-lab) has a particle demo inside a panel and
`BootIntro`, below.

## `ShaderView`

A `View` (layout, `style`, `className`, children) plus:

| Prop | Notes |
| --- | --- |
| `source` | Fragment shader, GLSL 4.10 core. A source without a `#version` line gets `#version 410 core`. Changing it compiles a new program. |
| `uniforms` | `{name: value}`: a number (`float`), 2 to 4 numbers (`vec2`–`vec4`), or a color string (`'#rgb'`, `'#rrggbb'`, `'#rrggbbaa'`, `'rgb()'`, `'rgba()'`) as a `vec4` in 0–1. At most 16. Updating them does not recompile. |
| `opacity` | 0–1, applied when the layer is composited (default 1). |
| `resolutionScale` | 0.05–1 (default 1): render into a smaller target and scale it up with linear filtering, for heavy shaders. 0.5 costs a quarter of the fragments. |
| `onError` | Called once with the GLSL info log when the shader fails to compile or link; without it the log goes to `console.error`. |

Uniforms the engine sets every frame, when the shader declares them:

| Uniform | Value |
| --- | --- |
| `float uTime` | Seconds since the layer was created (the `ShaderView` mounted or its `source` changed) |
| `vec2 uResolution` | Size of the layer's render target in pixels (the rect × `resolutionScale`) |
| `vec4 uRect` | The rect in framebuffer pixels: x, y (top-left), width, height |
| `float uOpacity` | The `opacity` prop; the engine already applies it when compositing |

`gl_FragCoord` is in render-target pixels with GL's origin at the bottom left, as
in WebGL; `gl_FragCoord.xy / uResolution` is 0–1 across the rect. Write one
`out vec4`; its alpha is straight (not premultiplied). The shader can use
anything GLSL 4.10 core offers (loops, functions, `discard`) but no
textures: the engine binds none.

A compile or link error logs `[PS5-REACT] shader <id>: <info log>` from the host,
reports the log through `onError`, and the layer draws nothing: the cutout stays
black. Nothing crashes, and the next `source` is compiled afresh.

## How it composites

The framebuffer the engine rasterizes stays the image of the UI; shader layers
are drawn by the presenter, which already owns the GPU:

1. `ShaderView` renders a `View` with the style `cutout: true`. When the engine
   paints that View, it first sets its clipped rect to transparent (`0x00000000`),
   then paints its background and children as usual
   (`patches/embeddedReact-view-cutout.patch`). The software backend's
   source-over composites alpha as well as color, so anything drawn over the hole
   keeps its own coverage and the framebuffer holds premultiplied color. Over an
   opaque pixel the result is bit for bit what it was, so an app without a
   `ShaderView` renders exactly as before.
2. Each frame, while any layer is live, the presenter clears the screen, renders
   each layer into its own target (the rect × `resolutionScale`) and composites
   it at the rect, in creation order. Then it draws the framebuffer over them
   with premultiplied blending (`ONE, ONE_MINUS_SRC_ALPHA`).

So UI painted after the `ShaderView` (its children, later siblings, overlays)
shows over the shader with its antialiasing intact, and the shader shows over
whatever the UI drew before it in that rect, because the cutout replaced those
pixels. A layer's rect, uniforms and source live in the host
(`__ps5ReactNative.shader`, [NATIVE-API.md](NATIVE-API.md#shaders-abi-v10)); its
`uTime` advances in the presenter, and the damage tracker never sees it. A
`ShaderView` costs the CPU nothing per frame beyond the presenter's draw calls.

With no live layer the presenter takes exactly its old path: no clear, no
blending.

## Limits

- **What is under a shader is replaced.** The cutout removes the UI beneath the
  rect, so a translucent shader output (or `opacity` below 1) blends with black,
  not with the UI behind it. Put backgrounds in the shader.
- **Rectangles only.** The cutout is the View's clipped rectangle; `borderRadius`
  does not round it (draw rounded corners in the shader with `uRect`).
- **No opacity groups or transforms around it.** Inside an ancestor with
  `opacity` below 1 or a transform, the engine paints into a scratch buffer and
  skips the cutout, so the shader is hidden. Use the `opacity` prop instead.
- **Laid-out rect.** The layer follows the View's `onLayout` rectangle, which is
  pre-scroll: a `ShaderView` inside a scrolled `ScrollView` draws at its unscrolled
  place. Keep shaders outside scrolling content.
- **Overlapping layers** draw in creation order, not tree order.
- At most 8 layers, 16 uniforms each, 64 KB of source.
- Screenshots: the preview's `shot` reads the frame back from the GPU while any
  layer is live (`ps5_drive.py shot`); the CPU framebuffer alone shows the
  cutouts as black.

## Cost

GPU time is the shader's fragment count times its per-pixel work; CPU time is
unchanged by animation. Measured in the desktop preview at 1920×1080 on an Apple M5
(`GL_TIME_ELAPSED` around the layers, 240-frame averages), the frame log showing
`raster=0.0 dirty=0kpx` while they animate:

| Scene | GPU | CPU raster | Frame rate |
| --- | --- | --- | --- |
| `BootIntro` scene, full screen | 4.3–4.6 ms | 0.0 ms (0.3 ms during the logo fade, 1.0 ms during the crossover) | 120 fps (display-bound) |
| shader-lab embers, 1368×516 panel | 1.2–1.8 ms | 0.0 ms | 120 fps |

PS5 GPU times are not measured yet. Use `resolutionScale` for heavy or soft
shaders (particles with blur, gradients); keep 1 for crisp specks and text-like
detail.

## `BootIntro`

The PS5 boot sequence as an app entrance: the logo on black, then the scene, then
the app crossing over it as the home screen does on the console. Timeline,
colors, particle sizes and speeds were measured from 1080p60 captures of the
console booting:

| Time (from the logo) | |
| --- | --- |
| 0–0.5 s | `logo` fades in on black (no capture shows this fade; 0.5 s is chosen) |
| 0.5–2.0 s | logo held (`logoHold`, 1.5 s) |
| 2.0–2.4 s | logo fades out (measured 0.27–0.43 s) |
| 3.2–6.2 s | a huge navy bokeh disc comes in from the right and grows (rgb 8,22,35 → 13,27,46) |
| 6.7–7.2 s | smaller teal discs over it |
| 7.0–7.6 s | a burst of warm dust (rgb 186,154,126, cores 240,220,200) spreading at 300–700 px/s over near black, motion-blurred specks, defocused bokeh 15–45 px wide on the right |
| 7.4–9.0 s | a beam from the top left (rgb 105,88,88 at the source), the room lifting to rgb 37,34,41 (15,14,26 on the right), haze over the floor (48,47,53); dust drifting right and up at 100–400 px/s with parallax |
| 9.05–9.5 s | `children` cross over the scene; `onDone` |

```jsx
import {BootIntro} from '@ps5-react/core';

<BootIntro logo={<MyLogo />} onDone={() => setReady(true)}>
  <Home />
</BootIntro>
```

A store's entrance, about 4.3 s: its mark (already on screen in the system splash), the dust and the beam, no bokeh, skippable:

```jsx
<BootIntro logo={<Mark />} logoIn={0} logoHold={1.6} skip={['bokeh']} speed={1.1} skippable>
  <Store />
</BootIntro>
```

| Prop | Notes |
| --- | --- |
| `logo` | Shown on black first. Without it the sequence starts at the bokeh. |
| `children` | The next screen, mounted when it starts crossing over the scene's last 0.45 s, and staying after it. Without children the scene fades to black. |
| `onDone` | Called when the scene is gone. |
| `logoIn` | Seconds the logo fades in (0.5); 0 shows it from the first frame, continuing a system splash that showed it. |
| `logoHold` | Seconds the logo stays (1.5). |
| `speed` | 1 is the console's pace (about 9.5 s with a logo); 2 halves every phase. |
| `skip` | Stages to leave out of `BOOT_INTRO_STAGES` (`logo`, `bokeh`, `burst`, `beam`): `'logo'` or `'bokeh'` (`[]`). Without the bokeh the scene opens 3.7 s in, where the burst's dark ground fades in, 0.15 s after the logo is gone, so the logo hands straight over to the dust; there is no blue at any point. The burst and the beam are one shot and cannot be skipped. |
| `skippable` | Cross, Circle or Options cut to `children` (a 0.25 s crossover) (`false`). |
| `resolutionScale` | Passed to the scene's `ShaderView` (1). |

The scene is one `ShaderView` (`bootIntroShader`): bokeh discs, a room lit by a
beam, and four depth layers of dust on jittered grids (one particle in some cells,
3×3 cells searched per pixel), spread out from a center for the burst and drifting
with parallax after it. Only the logo fade and the crossover make the CPU repaint.
The console's health notice and the PlayStation logo are not part of it: pass your
own logo.
