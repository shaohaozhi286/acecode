import assert from 'node:assert/strict';
import {
  IMAGE_NORMALIZE_MAX_EDGE,
  IMAGE_NORMALIZE_THRESHOLD_BYTES,
  shouldNormalizeImage,
} from './imageNormalize.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (e) {
    console.error(`[fail] ${name}`);
    console.error(e);
    process.exitCode = 1;
  }
}

await run('10MiB threshold triggers image normalization', () => {
  assert.equal(shouldNormalizeImage({
    size: IMAGE_NORMALIZE_THRESHOLD_BYTES + 1,
    width: 1200,
    height: 800,
  }), true);
});

await run('exactly 10MiB triggers by size', () => {
  assert.equal(shouldNormalizeImage({
    size: IMAGE_NORMALIZE_THRESHOLD_BYTES,
    width: 1200,
    height: 800,
  }), true);
});

await run('max edge triggers image normalization even below 10MiB', () => {
  assert.equal(shouldNormalizeImage({
    size: 1024,
    width: IMAGE_NORMALIZE_MAX_EDGE + 1,
    height: 1000,
  }), true);
});
