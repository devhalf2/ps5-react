// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// BootIntro's timeline, in seconds of the reference captures (PS5 boot at 1080p60). The logo is on
// black: it fades out in 0.27-0.43 s; no capture shows it fading in (they start with it on), so 0.5 s is
// chosen. The scene starts sceneLead after the logo is gone; scene time 0 is when the navy bokeh appears.
//   scene 0.0-3.0  a huge navy bokeh disc enters from the right and grows (rgb 8,22,35 -> 13,27,46)
//   3.5-3.95       smaller teal discs over it
//   3.8-4.4        a burst of warm dust (rgb 186,154,126, cores 240,220,200) spreads at 300-700 px/s
//                  over near black, motion-blurred specks, defocused bokeh 15-45 px wide on the right
//   4.2-6.0        a beam from the top left (rgb 105,88,88 at the source), the room lifting to rgb
//                  37,34,41 (15,14,26 on the right), haze over the floor (48,47,53); dust drifts right
//                  and up at 100-400 px/s with parallax, fainter and fewer as the scene settles
//   5.85-6.3       the next screen crosses over the scene (the home screen on the console)
export const BOOT_INTRO_TIMING = {logoIn: 0.5, logoHold: 1.5, logoOut: 0.4, sceneLead: 0.8, scene: 5.85, reveal: 0.45};

// The stages in their order; `skip` leaves some out. Only the logo and the bokeh can go: the burst and
// the beam are one continuous shot. Without the bokeh the scene starts just before the burst
// (BOKEH_SKIP_START of scene time, where the burst's dark ground fades in) after a shorter lead, so
// the logo hands straight over to the dust.
export const BOOT_INTRO_STAGES = ['logo', 'bokeh', 'burst', 'beam'];
const SKIPPABLE = new Set(['logo', 'bokeh']);
export const BOKEH_SKIP_START = 3.7;
const BOKEH_SKIP_LEAD = 0.15;

/**
 * When each phase starts, in seconds from mount, at `speed` (2 plays twice as fast). `start` is the
 * scene time the scene opens at (0 with the bokeh).
 * @param {{logo: boolean, logoIn?: number, logoHold?: number, speed?: number, skip?: string[]}} options
 * @returns {{logoOut: number, lead: number, scene: number, reveal: number, done: number, start: number}}
 */
export function bootIntroSchedule({logo, logoIn = BOOT_INTRO_TIMING.logoIn, logoHold = BOOT_INTRO_TIMING.logoHold, speed = 1,
  skip = []}) {
  if (!(speed > 0) || !(logoHold >= 0) || !(logoIn >= 0)) {
    throw new RangeError(`BootIntro: speed must be above 0, logoIn and logoHold at least 0`);
  }
  for (const stage of skip) {
    if (!SKIPPABLE.has(stage)) throw new RangeError(`BootIntro: cannot skip ${JSON.stringify(stage)} (only ${[...SKIPPABLE].join(', ')})`);
  }
  const T = BOOT_INTRO_TIMING, s = 1 / speed;
  const bokeh = !skip.includes('bokeh');
  if (skip.includes('logo')) logo = false;
  const start = bokeh ? 0 : BOKEH_SKIP_START;
  const logoOut = logo ? (logoIn + logoHold) * s : 0;
  const lead = logo ? logoOut + T.logoOut * s : 0;
  const scene = logo ? lead + (bokeh ? T.sceneLead : BOKEH_SKIP_LEAD) * s : 0;
  const reveal = scene + (T.scene - start) * s;
  return {logoOut, lead, scene, reveal, done: reveal + T.reveal * s, start};
}
