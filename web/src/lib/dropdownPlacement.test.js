import assert from 'node:assert/strict';
import {
  computeAnchoredDropdownLayout,
  DROPDOWN_GAP_PX,
  DROPDOWN_VIEWPORT_MARGIN_PX,
} from './dropdownPlacement.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const defaults = {
  viewportTop: 0,
  viewportHeight: 800,
  preferredHeight: 240,
};

run('prefers above when both sides fit', () => {
  assert.deepEqual(
    computeAnchoredDropdownLayout({
      ...defaults,
      anchorTop: 400,
      anchorBottom: 440,
    }),
    {
      placement: 'above',
      availableAbove: 384,
      availableBelow: 344,
      maxHeight: 240,
      constrained: false,
    },
  );
});

run('falls below when above is short and below fits', () => {
  const layout = computeAnchoredDropdownLayout({
    ...defaults,
    anchorTop: 120,
    anchorBottom: 160,
  });
  assert.equal(layout.placement, 'below');
  assert.equal(layout.maxHeight, 240);
  assert.equal(layout.constrained, false);
});

run('uses the larger upper side and constrains height when neither side fits', () => {
  const layout = computeAnchoredDropdownLayout({
    ...defaults,
    anchorTop: 430,
    anchorBottom: 470,
    preferredHeight: 500,
  });
  assert.equal(layout.placement, 'above');
  assert.equal(layout.maxHeight, 414);
  assert.equal(layout.constrained, true);
});

run('uses the larger lower side and constrains height when neither side fits', () => {
  const layout = computeAnchoredDropdownLayout({
    ...defaults,
    anchorTop: 300,
    anchorBottom: 340,
    preferredHeight: 500,
  });
  assert.equal(layout.placement, 'below');
  assert.equal(layout.maxHeight, 444);
  assert.equal(layout.constrained, true);
});

run('keeps the above preference when constrained sides tie', () => {
  const layout = computeAnchoredDropdownLayout({
    ...defaults,
    anchorTop: 380,
    anchorBottom: 420,
    preferredHeight: 500,
  });
  assert.equal(layout.availableAbove, layout.availableBelow);
  assert.equal(layout.placement, 'above');
  assert.equal(layout.maxHeight, 364);
});

run('honors a visual viewport offset', () => {
  const layout = computeAnchoredDropdownLayout({
    anchorTop: 600,
    anchorBottom: 640,
    viewportTop: 100,
    viewportHeight: 600,
    preferredHeight: 450,
  });
  assert.equal(layout.availableAbove, 484);
  assert.equal(layout.availableBelow, 44);
  assert.equal(layout.placement, 'above');
  assert.equal(layout.maxHeight, 450);
});

run('custom gap and margin are included in both available spaces', () => {
  const layout = computeAnchoredDropdownLayout({
    anchorTop: 250,
    anchorBottom: 300,
    viewportHeight: 600,
    preferredHeight: 100,
    gap: 10,
    margin: 20,
  });
  assert.equal(layout.availableAbove, 220);
  assert.equal(layout.availableBelow, 270);
});

run('invalid and negative geometry never produces NaN or a negative height', () => {
  const layout = computeAnchoredDropdownLayout({
    anchorTop: Number.NaN,
    anchorBottom: Number.NEGATIVE_INFINITY,
    viewportHeight: -1,
    preferredHeight: Number.POSITIVE_INFINITY,
    gap: -10,
    margin: -10,
  });
  assert.deepEqual(layout, {
    placement: 'above',
    availableAbove: 0,
    availableBelow: 0,
    maxHeight: 0,
    constrained: false,
  });
});

run('default spacing constants remain aligned with the composer gap', () => {
  assert.equal(DROPDOWN_GAP_PX, 8);
  assert.equal(DROPDOWN_VIEWPORT_MARGIN_PX, 8);
});
