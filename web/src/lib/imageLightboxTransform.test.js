import assert from 'node:assert/strict';

import {
  clampLightboxScale,
  clampLightboxTransform,
  LIGHTBOX_MAX_SCALE,
  LIGHTBOX_MIN_SCALE,
  lightboxCanPan,
  lightboxPanLimits,
  panLightboxTransform,
  zoomLightboxTransform,
} from './imageLightboxTransform.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('lightbox scale is finite and bounded', () => {
  assert.equal(clampLightboxScale(Number.NaN), LIGHTBOX_MIN_SCALE);
  assert.equal(clampLightboxScale(0.25), LIGHTBOX_MIN_SCALE);
  assert.equal(clampLightboxScale(3.5), 3.5);
  assert.equal(clampLightboxScale(20), LIGHTBOX_MAX_SCALE);
});

run('lightbox transform clamps each overflowed edge', () => {
  const metrics = {
    imageWidth: 800,
    imageHeight: 600,
    viewportWidth: 400,
    viewportHeight: 300,
  };
  assert.deepEqual(lightboxPanLimits({ scale: 2 }, metrics), { x: 600, y: 450 });
  assert.deepEqual(
    clampLightboxTransform({ scale: 2, x: 1000, y: -1000 }, metrics),
    { scale: 2, x: 600, y: -450 },
  );
});

run('lightbox keeps a non-overflowed axis centered', () => {
  const metrics = {
    imageWidth: 100,
    imageHeight: 400,
    viewportWidth: 500,
    viewportHeight: 300,
  };
  assert.deepEqual(
    clampLightboxTransform({ scale: 2, x: 80, y: 400 }, metrics),
    { scale: 2, x: 0, y: 250 },
  );
  assert.equal(lightboxCanPan({ scale: 1 }, metrics), true);
  assert.equal(lightboxCanPan({ scale: 1 }, {
    imageWidth: 100,
    imageHeight: 100,
    viewportWidth: 500,
    viewportHeight: 300,
  }), false);
});

run('pointer-anchored zoom retains the inspected image point', () => {
  const metrics = {
    imageWidth: 400,
    imageHeight: 200,
    viewportWidth: 400,
    viewportHeight: 300,
  };
  assert.deepEqual(
    zoomLightboxTransform(
      { scale: 1, x: 0, y: 0 },
      2,
      metrics,
      { x: 300, y: 150 },
    ),
    { scale: 2, x: -100, y: 0 },
  );
});

run('drag deltas move zoomed content and stay inside pan limits', () => {
  const metrics = {
    imageWidth: 400,
    imageHeight: 300,
    viewportWidth: 400,
    viewportHeight: 300,
  };
  assert.deepEqual(
    panLightboxTransform(
      { scale: 2, x: -50, y: 25 },
      { x: 1000, y: -1000 },
      metrics,
    ),
    { scale: 2, x: 200, y: -150 },
  );
});
