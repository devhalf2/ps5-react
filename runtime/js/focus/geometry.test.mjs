// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {anchorOffset, catchUpSpeed, easeOutAt, findClosest, findNearest, findWrap, revealOffset, scrollStep, snapRevealOffset} from './geometry.js';

const rect = (x, y, width = 100, height = 100) => ({x, y, width, height});
const grid = () => {
  const cells = [];
  for (let row = 0; row < 3; row++) {
    for (let column = 0; column < 3; column++) {
      cells.push({id: `${row}${column}`, rect: rect(column * 120, row * 120), order: cells.length});
    }
  }
  return cells;
};
const move = (cells, id, direction) => {
  const from = cells.find(cell => cell.id === id);
  return findNearest(from.rect, cells.filter(cell => cell !== from), direction)?.id ?? null;
};

test('a grid moves one cell per direction and stops at the edges', () => {
  const cells = grid();
  assert.equal(move(cells, '11', 'up'), '01');
  assert.equal(move(cells, '11', 'down'), '21');
  assert.equal(move(cells, '11', 'left'), '10');
  assert.equal(move(cells, '11', 'right'), '12');
  assert.equal(move(cells, '00', 'up'), null);
  assert.equal(move(cells, '00', 'left'), null);
  assert.equal(move(cells, '22', 'right'), null);
});

test('orthogonal offset weighs double the primary distance', () => {
  const from = rect(0, 0);
  const aligned = {rect: rect(400, 0), order: 0};
  const diagonal = {rect: rect(150, 200), order: 1};
  // aligned: 300 + 0; diagonal: 50 + 2 × 100.
  assert.equal(findNearest(from, [aligned, diagonal], 'right'), diagonal);
  const far = {rect: rect(150, 300), order: 1};
  assert.equal(findNearest(from, [aligned, far], 'right'), aligned);
});

test('ties prefer more orthogonal overlap, then document order', () => {
  const from = rect(0, 0, 100, 100);
  const partial = {rect: rect(0, 120, 100, 100), order: 0};
  const shifted = {rect: rect(50, 120, 100, 100), order: 1};
  assert.equal(findNearest(from, [shifted, partial], 'down'), partial);
  const first = {rect: rect(0, 120), order: 0};
  const second = {rect: rect(0, 120), order: 1};
  assert.equal(findNearest(from, [second, first], 'down'), first);
});

test('an element containing the origin or behind it is not a candidate', () => {
  const from = rect(100, 100);
  assert.equal(findNearest(from, [{rect: rect(0, 0, 400, 400), order: 0}], 'right'), null);
  assert.equal(findNearest(from, [{rect: rect(50, 100), order: 0}], 'right'), null);
  assert.ok(findNearest(from, [{rect: rect(150, 100), order: 0}], 'right'));
});

test('wrap picks the far end of the same row or column', () => {
  const cells = grid();
  const at = id => cells.find(cell => cell.id === id);
  const others = id => cells.filter(cell => cell.id !== id);
  assert.equal(findWrap(at('12').rect, others('12'), 'right').id, '10');
  assert.equal(findWrap(at('10').rect, others('10'), 'left').id, '12');
  assert.equal(findWrap(at('21').rect, others('21'), 'down').id, '01');
  assert.equal(findWrap(at('00').rect, [], 'right'), null);
});

test('closest by centre distance', () => {
  const cells = grid();
  assert.equal(findClosest(rect(250, 250, 20, 20), cells).id, '22');
});

test('reveal scrolls the least, keeps a margin, and clamps', () => {
  assert.equal(revealOffset(0, 100, 50, 300, 1000, 10), 0);
  assert.equal(revealOffset(0, 400, 50, 300, 1000, 10), 160);
  assert.equal(revealOffset(500, 400, 50, 300, 1000, 10), 390);
  assert.equal(revealOffset(0, 5, 50, 300, 1000, 10), 0);
  assert.equal(revealOffset(0, 1400, 50, 300, 1000, 10), 1000);
  assert.equal(revealOffset(0, 400, 500, 300, 1000, 10), 390);
});

/** Per-frame steps of a scroll from `from` to `to`, retargeting by `extra` px every `every` frames. */
function steps(from, to, {extra = 0, every = 0, frames = 200} = {}) {
  let position = from, speed = 0, target = to;
  const deltas = [];
  for (let frame = 1; frame <= frames && (position !== target || (every && frame < frames)); frame++) {
    if (every && frame % every === 0) target += extra;
    const [next, nextSpeed] = scrollStep(position, speed, target, 60, 18, 6);
    deltas.push(next - position);
    [position, speed] = [next, nextSpeed];
  }
  return {position, deltas};
}

test('a scroll step speeds up, cruises and brakes to land exactly in whole pixels', () => {
  const {position, deltas} = steps(0, 1000);
  assert.equal(position, 1000);
  assert.deepEqual(deltas.slice(0, 4), [18, 36, 54, 60]);
  assert.ok(deltas.every(Number.isInteger));
  // Braking never speeds up again and ends without a tail of tiny steps.
  const braking = deltas.slice(deltas.lastIndexOf(60) + 1);
  assert.ok(braking.every((delta, i) => i === 0 || delta <= braking[i - 1]));
  assert.ok(braking.length <= 11);
  assert.deepEqual(steps(1000, 0).deltas.slice(0, 2), [-18, -36]);
  assert.deepEqual(steps(0, 3).deltas, [3]);
});

test('a target that keeps moving ahead scrolls at one steady speed', () => {
  // A held key: one 421 px row every 6 frames, faster than the cap.
  const {deltas} = steps(0, 421, {extra: 421, every: 6, frames: 70});
  assert.ok(deltas.slice(6).every(delta => delta >= 58 && delta <= 60), deltas.join());
});

test('an anchor aligns its start past the margin, scrolls to 0 near the top, and never hides the item', () => {
  // A section at 600 whose card sits at 650: the title stays on screen, not just the card.
  assert.equal(anchorOffset(0, 650, 100, 600, 300, 1000, 10), 590);
  assert.equal(anchorOffset(800, 650, 100, 600, 300, 1000, 10), 590);
  // The hero at the top of the content scrolls all the way back to 0.
  assert.equal(anchorOffset(400, 200, 100, 5, 300, 1000, 10), 0);
  assert.equal(anchorOffset(0, 1300, 50, 1200, 300, 1000, 10), 1000);
  // An anchor taller than the viewport: the item far below its start moves the least instead.
  assert.equal(anchorOffset(0, 700, 50, 100, 300, 1000, 10), revealOffset(0, 700, 50, 300, 1000, 10));
});

test('reveal keeps a margin of its own at each edge', () => {
  // [before, after]: 40 px stays clear below the item, 10 above it.
  assert.equal(revealOffset(0, 400, 50, 300, 1000, [10, 40]), 190);
  assert.equal(revealOffset(500, 400, 50, 300, 1000, [10, 40]), 390);
  assert.equal(revealOffset(0, 400, 50, 300, 1000, [10, 10]), revealOffset(0, 400, 50, 300, 1000, 10));
  // An item larger than the viewport still aligns its start past the start margin.
  assert.equal(revealOffset(0, 400, 500, 300, 1000, [10, 40]), 390);
});

test('an anchor aligns past the start margin and, given an end, aligns only if the item clears it', () => {
  assert.equal(anchorOffset(0, 650, 100, 600, 300, 1000, [50, 10]), 550);
  assert.equal(anchorOffset(0, 650, 100, 600, 300, 1000, [10, 10], 100), 590);
  // 200 px at the end would cover the card: it moves the least instead, by both margins.
  assert.equal(anchorOffset(0, 650, 100, 600, 300, 1000, [10, 10], 200), 460);
});

test('a snapping reveal lands on row starts, and moves the least when none shows the item', () => {
  // 48 px rows from 4 px down in a 392 px viewport (8 rows and 4 px either side), 4 px margins.
  const rows = Array.from({length: 20}, (_, i) => 4 + i * 48);
  assert.equal(snapRevealOffset(0, 388, 48, 392, 1000, 4, rows), 48);
  assert.equal(snapRevealOffset(48, 100, 48, 392, 1000, 4, rows), 48, 'a row in view does not move');
  assert.equal(snapRevealOffset(96, 52, 48, 392, 1000, 4, rows), 48);
  assert.equal(snapRevealOffset(48, 4, 48, 392, 1000, 4, rows), 0);
  // Cards of two heights, 65 px kept clear at the bottom: the card start nearest the 159 px of least
  // movement that still shows the focused card whole.
  const cards = [0, 200, 392, 529];
  assert.equal(snapRevealOffset(0, 529, 125, 560, 2000, [0, 65], cards), 200);
  assert.equal(snapRevealOffset(200, 0, 167, 560, 2000, [0, 65], cards), 0);
  // A card taller than the viewport aligns its start as without snapping.
  assert.equal(snapRevealOffset(0, 100, 900, 560, 2000, [0, 65], [100]), 100);
  assert.equal(snapRevealOffset(0, 1000, 125, 560, 2000, [0, 65], [300, 1100]), 630, 'none fits: least movement');
  assert.equal(snapRevealOffset(0, 388, 48, 392, 30, 4, rows), 30, 'clamped to the content');
});

test('a snapping reveal into a section lands on the section row\'s own top, not the gap before it', () => {
  // Downloads: sections 40 px apart, each row an anchor, a section's label inside its first row's.
  // Section A: rows at 0 (label + card, 160) and 160 (140); section B from 340 (label + card, 160).
  const starts = [0, 160, 340];
  // Down onto B's first row in a 380 px viewport, 65 px clear at the bottom: neither A row start shows
  // it whole, so B's label goes to the top (340), not 40 px down after A's end (300).
  assert.equal(snapRevealOffset(0, 340, 160, 380, 2000, [0, 65], starts), 340);
  // Up into A's last row from there: its own top, not B's gap.
  assert.equal(snapRevealOffset(340, 160, 140, 380, 2000, [0, 65], starts), 160);
});

test('a scroll far behind its target is allowed faster than the cap, near it the cap holds', () => {
  assert.equal(catchUpSpeed(300, 1080, 60), 60);
  assert.equal(catchUpSpeed(-300, 1080, 60), 60);
  assert.equal(catchUpSpeed(1360, 1080, 60), 250);
  assert.equal(catchUpSpeed(-1360, 1080, 60), 250);
});

test('an eased-out scroll is a quarter done in one frame, slows, and rests on the target at its end', () => {
  const frame = 1000 / 60;
  const at = Array.from({length: 15}, (_, i) => easeOutAt(0, 420, (i + 1) * frame, 240));
  assert.deepEqual(at, [105, 189, 255, 306, 344, 371, 391, 404, 412, 416, 419, 420, 420, 420, 420]);
  assert.ok(at.every((y, i) => i === 0 || y - at[i - 1] <= at[i - 1] - (at[i - 2] ?? 0)), 'never speeds up');
  assert.equal(easeOutAt(0, 420, 400, 240), 420, 'never past the target');
  assert.equal(easeOutAt(420, 0, 2 * frame, 240), 231, 'and backwards');
  assert.equal(easeOutAt(100, 100, 50, 240), 100);
});
