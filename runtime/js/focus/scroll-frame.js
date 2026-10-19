// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The scrolling state behind a ScrollView: focus reveals, the eased per-frame scroll through
// NativeUI.scrollTo (patches/embeddedReact-scroll-to.patch) and `onScrollTarget`. Free of React so
// tests can drive it with a stand-in NativeUI.
import {onFrame} from '../frame.js';
import {deferImageFetches} from '../native.js';
import {anchorOffset, catchUpSpeed, easeOutAt, revealOffset, scrollStep, snapRevealOffset} from './geometry.js';

// The reveal margin on each edge without `revealInsets`: 32 logical px of a 1280-wide layout.
const MARGIN = 32;
// The engine cannot animate a scroll offset, so JavaScript steps it on every presented frame (the
// host's frame callback, not a timer that drifts against the display), in whole pixels: up to 40
// logical px per 60 Hz frame, reached in four frames, then braking to stop on the target. A held
// key scrolls at that one steady speed, which also bounds the strip the engine rasterizes per frame.
const MAX_LOGICAL_PX_PER_FRAME = 40;
const ACCEL_LOGICAL_PX = 12;
const BRAKE_LOGICAL_PX = 4;
const FRAME_MS = 1000 / 60;
// `scrollMotion="ease-out"`, the PS5 Store's feel: a quartic ease-out over 240 ms from where the view is
// to the target (25% after one 60 Hz frame, 73% after four, on it in whole pixels after twelve: 200 ms),
// by the time each frame advances. Measured frame by frame (full range, m7o_5teJL7s.mp4 at 60 fps and nItrEsIBD5Y.mp4 at
// 30 fps), the store's rail steps fit a quartic over 235-238 ms (a cubic fits them worse at 190-198 ms),
// its row-to-row moves a cubic over 205 ms or a quartic over 250; either way they show motion for
// 180-200 ms. A new target starts a new 240 ms from where the view is then.
const EASE_MS = 240;
// Images wait while the view moves and this long after it stops, like a browser's lazy loading: art
// that only flies past is never fetched; what the view stops on (with the rows the list mounts ahead)
// starts then. Cached art still draws during the scroll.
const IMAGE_DEFER_MS = 150;
// A focus scroll farther than this many viewports (Circle back to the top of a long catalog) would pass
// every row on the way, at a catch-up speed that mounts and rasterizes screens of content per frame. It
// jumps to one viewport short of the target instead, and only that last viewport moves.
const FAR_VIEWPORTS = 2;

/** Where a scroll from `from` to `to` starts moving: `from`, or one `viewport` short of a far `to`. */
function startOf(from, to, viewport) {
  return Math.abs(to - from) > FAR_VIEWPORTS * viewport ? Math.round(to + Math.sign(from - to) * viewport) : from;
}

function assignRef(ref, value) {
  if (typeof ref === 'function') ref(value);
  else if (ref) ref.current = value;
}

/** @param {import('./manager.js').Frame | null} parent */
export function createFrame(parent) {
  const frame = {
    parent, x: 0, y: 0, props: {}, forwardedRef: null, handle: null, viewport: null, timer: null, target: null,
    speed: [0, 0], position: [0, 0], ease: {from: [0, 0], elapsed: 0}, listeners: new Set(),
    /** @type {Set<{rect: import('./geometry.js').Rect | null}>} the scroll anchors directly inside */
    anchors: new Set(),
    /**
     * @param {import('./geometry.js').Rect} rect pre-scroll, like `viewport`
     * @param {import('./geometry.js').Rect | null} anchor its `scrollAnchor` ancestor in this frame, aligned
     *   on a vertical frame's y axis; the x axis and horizontal frames always move the least
     */
    reveal(rect, anchor) {
      const {viewport, handle} = frame;
      if (!viewport || handle == null) return;
      let [fromX, fromY, maxX, maxY] = NativeUI.scrollTo(handle, NaN, NaN);
      // Logical px to screen px, like the layout rectangles.
      const scale = screen.width / 1280;
      const insets = frame.props.revealInsets;
      const inset = edge => Math.round((insets?.[edge] ?? MARGIN) * scale);
      const [top, bottom, left, right] = ['top', 'bottom', 'left', 'right'].map(inset);
      const vertical = !frame.props.horizontal;
      // With `revealSnap`, the scrolling axis lands on the starts of the frame's scroll anchors.
      const snap = Boolean(frame.props.revealSnap);
      const starts = axis => {
        const result = [];
        for (const {rect: row} of frame.anchors) if (row) result.push(axis === 'x' ? row.x - viewport.x : row.y - viewport.y);
        return result;
      };
      const x = snap && !vertical
        ? snapRevealOffset(frame.x, rect.x - viewport.x, rect.width, viewport.width, maxX, [left, right], starts('x'))
        : revealOffset(frame.x, rect.x - viewport.x, rect.width, viewport.width, maxX, [left, right]);
      let y;
      if (snap && vertical) {
        // The anchor is the row: it shows whole, instead of aligning.
        const row = anchor ?? rect;
        y = snapRevealOffset(frame.y, row.y - viewport.y, row.height, viewport.height, maxY, [top, bottom], starts('y'));
      } else if (vertical && anchor) {
        // An explicit bottom inset is an overlay there: an anchor aligns only if the item then clears it.
        y = anchorOffset(frame.y, rect.y - viewport.y, rect.height, anchor.y - viewport.y, viewport.height, maxY,
          [top, bottom], insets?.bottom != null ? bottom : 0);
      } else y = revealOffset(frame.y, rect.y - viewport.y, rect.height, viewport.height, maxY, [top, bottom]);
      if (x === frame.x && y === frame.y) return;
      const [startX, startY] = [startOf(fromX, x, viewport.width), startOf(fromY, y, viewport.height)];
      if (startX !== fromX || startY !== fromY) {
        frame.stop();
        [fromX, fromY] = NativeUI.scrollTo(handle, startX, startY);
      }
      frame.x = x;
      frame.y = y;
      frame.animate(fromX, fromY, x, y);
      frame.notify();
      frame.props.onScrollTarget?.({x, y});
    },
    // A new target while scrolling only moves the target: steady steps carry on at the current speed, an
    // ease-out starts again from where the view is.
    animate(fromX, fromY, toX, toY) {
      frame.target = [Math.round(toX), Math.round(toY)];
      const running = frame.timer !== null;
      frame.ease = {from: running ? [...frame.position] : [Math.round(fromX), Math.round(fromY)], elapsed: 0};
      if (running) return;
      const scale = screen.width / 1280;
      const [maxSpeed, accel, brake] = [MAX_LOGICAL_PX_PER_FRAME, ACCEL_LOGICAL_PX, BRAKE_LOGICAL_PX]
        .map(logical => Math.max(1, Math.round(logical * scale)));
      frame.position = [Math.round(fromX), Math.round(fromY)];
      frame.speed = [0, 0];
      frame.timer = onFrame(elapsedMs => {
        let [x, y] = frame.position;
        if (frame.motion() === 'ease-out') {
          // At most two vblanks' worth, so a hitch is not followed by a jump.
          const {ease} = frame;
          ease.elapsed += elapsedMs > 0 ? Math.min(2 * FRAME_MS, elapsedMs) : FRAME_MS;
          const [nextX, nextY] = [easeOutAt(ease.from[0], frame.target[0], ease.elapsed, EASE_MS),
            easeOutAt(ease.from[1], frame.target[1], ease.elapsed, EASE_MS)];
          frame.speed = [nextX - x, nextY - y];
          [x, y] = [nextX, nextY];
        }
        // A late frame advances the vblanks it covered, at most two, so a hitch is not followed by a jump.
        else for (let n = Math.min(2, Math.max(1, Math.round(elapsedMs / FRAME_MS))); n > 0; n--) {
          let speedX, speedY;
          const capX = catchUpSpeed(frame.target[0] - x, frame.viewport.width, maxSpeed);
          const capY = catchUpSpeed(frame.target[1] - y, frame.viewport.height, maxSpeed);
          [x, speedX] = scrollStep(x, frame.speed[0], frame.target[0], capX, accel, brake);
          [y, speedY] = scrollStep(y, frame.speed[1], frame.target[1], capY, accel, brake);
          frame.speed = [speedX, speedY];
        }
        const [atX, atY] = NativeUI.scrollTo(frame.handle, x, y);
        deferImageFetches(IMAGE_DEFER_MS);
        // The engine stores offsets as floats. Far from the request means clamped by a content size that
        // changed since the target was chosen: settle where it stopped.
        if (Math.abs(atX - x) > 0.5) frame.target[0] = frame.ease.from[0] = x = Math.round(atX);
        if (Math.abs(atY - y) > 0.5) frame.target[1] = frame.ease.from[1] = y = Math.round(atY);
        frame.position = [x, y];
        if (x !== frame.target[0] || y !== frame.target[1]) return;
        frame.stop();
        const clamped = x !== frame.x || y !== frame.y;
        frame.x = x;
        frame.y = y;
        frame.notify();
        if (clamped) frame.props.onScrollTarget?.({x, y});
      });
    },
    /**
     * The content from offset `top` down moved up by `dy` (a VirtualList dropping rows above the
     * viewport, or down for a negative `dy`): an offset there scrolls by the same amount so nothing
     * moves on screen, mid-animation included. An offset above `top`, on content that stayed (a
     * header above the list), stays too: else a scroll to the top would be carried back into the list.
     */
    shift(dy, top = -Infinity) {
      const moved = offset => (offset >= top ? offset - dy : offset);
      const [, atY] = NativeUI.scrollTo(frame.handle, NaN, NaN);
      const here = atY >= top;
      if (here) NativeUI.scrollTo(frame.handle, NaN, atY - dy);
      frame.y = moved(frame.y);
      if (frame.timer === null) return;
      // The view and the start of its ease move as the engine's offset did; the target on its own.
      if (here) {
        frame.position[1] -= dy;
        frame.ease.from[1] -= dy;
      }
      frame.target[1] = moved(frame.target[1]);
    },
    /** Whether this view or one around it is focus scrolling. @returns {boolean} */
    scrolling() {
      for (let at = frame; at; at = at.parent) if (at.timer !== null) return true;
      return false;
    },
    /** `scrollMotion`, else the enclosing ScrollView's, else 'steady'. @returns {string} */
    motion() {
      for (let at = frame; at; at = at.parent) if (at.props.scrollMotion) return at.props.scrollMotion;
      return 'steady';
    },
    // Viewport or offset changed; while animating, y is the target, so listeners see where it is going.
    notify() {
      for (const listener of frame.listeners) listener();
    },
    stop() {
      frame.timer?.();
      frame.timer = null;
    },
    onLayout(event) {
      frame.viewport = event.layout;
      frame.notify();
      frame.props.onLayout?.(event);
    },
    // While focus scrolling animates, x/y already hold its target; touch scrolling updates them live.
    onScroll(event) {
      if (frame.timer === null) {
        frame.x = event.scrollX;
        frame.y = event.scrollY;
        frame.notify();
      }
      frame.props.onScroll?.(event);
    },
    ref(handle) {
      frame.handle = handle;
      assignRef(frame.forwardedRef, handle);
    },
  };
  return frame;
}
