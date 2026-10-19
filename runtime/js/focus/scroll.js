// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A ScrollView that keeps its focused descendant in view by scrolling natively through
// NativeUI.scrollTo (patches/embeddedReact-scroll-to.patch). Layout rectangles stay pre-scroll; the
// focus manager subtracts each enclosing frame's offset.
import {createElement, forwardRef, useContext, useLayoutEffect, useReducer, useState} from 'react';
import {ScrollView as HostScrollView} from 'embedded-react';
import {FrameContext} from './runtime.js';
import {createFrame} from './scroll-frame.js';

/**
 * Embedded React's ScrollView, plus scrolling its focused descendant into view (minimal movement
 * with a margin, or aligned to its `scrollAnchor` ancestor; eased and speed-capped).
 * `onScrollTarget({x, y})` reports each new focus-scroll target offset, when the scroll starts.
 * `revealInsets={{top, bottom, left, right}}` (logical px, each 32 when left out) is the room kept past
 * the focused element on each edge; `revealSnap` makes focus scrolling land on the starts of its
 * `scrollAnchor` rows (the top inset past the viewport top), so only whole rows show there. `scrollMotion="ease-out"` eases
 * focus scrolling out over 120 ms instead of the default steady speed; nested ScrollViews without
 * their own inherit it.
 */
export const ScrollView = forwardRef((props, ref) => {
  const parent = useContext(FrameContext);
  const [frame] = useState(() => createFrame(parent));
  frame.props = props;
  frame.forwardedRef = ref;
  useLayoutEffect(() => () => frame.stop(), []);
  const {children, onScrollTarget, revealInsets, revealSnap, scrollMotion, ...rest} = props;
  return createElement(HostScrollView, {...rest, ref: frame.ref, onLayout: frame.onLayout, onScroll: frame.onScroll},
    createElement(FrameContext.Provider, {value: frame}, children));
});
ScrollView.displayName = 'ScrollView';

/**
 * Whether the ScrollView around the caller, or one around that, is focus scrolling: true from the
 * focus move that starts a scroll until the view rests on its target. Re-renders only when that
 * changes. False outside any ScrollView. Touch scrolling does not count.
 */
export function useIsScrolling() {
  const frame = useContext(FrameContext);
  const [, rerender] = useReducer(n => n + 1, 0);
  const scrolling = frame ? frame.scrolling() : false;
  useLayoutEffect(() => {
    if (!frame) return undefined;
    const frames = [];
    for (let at = frame; at; at = at.parent) frames.push(at);
    let last = scrolling;
    const listener = () => {
      const now = frame.scrolling();
      if (now !== last) { last = now; rerender(); }
    };
    for (const at of frames) at.listeners.add(listener);
    listener();
    return () => { for (const at of frames) at.listeners.delete(listener); };
  }, [frame]);
  return scrolling;
}
