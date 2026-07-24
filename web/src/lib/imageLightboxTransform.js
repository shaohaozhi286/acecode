export const LIGHTBOX_MIN_SCALE = 1;
export const LIGHTBOX_MAX_SCALE = 8;

function finiteNumber(value, fallback = 0) {
  return Number.isFinite(value) ? value : fallback;
}

function positiveNumber(value) {
  return Math.max(0, finiteNumber(value));
}

function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

export function clampLightboxScale(value) {
  return clamp(
    finiteNumber(value, LIGHTBOX_MIN_SCALE),
    LIGHTBOX_MIN_SCALE,
    LIGHTBOX_MAX_SCALE,
  );
}

export function lightboxPanLimits(transform, metrics) {
  const scale = clampLightboxScale(transform?.scale);
  const imageWidth = positiveNumber(metrics?.imageWidth);
  const imageHeight = positiveNumber(metrics?.imageHeight);
  const viewportWidth = positiveNumber(metrics?.viewportWidth);
  const viewportHeight = positiveNumber(metrics?.viewportHeight);
  return {
    x: Math.max(0, ((imageWidth * scale) - viewportWidth) / 2),
    y: Math.max(0, ((imageHeight * scale) - viewportHeight) / 2),
  };
}

export function clampLightboxTransform(transform, metrics) {
  const scale = clampLightboxScale(transform?.scale);
  const limits = lightboxPanLimits({ scale }, metrics);
  return {
    scale,
    x: clamp(finiteNumber(transform?.x), -limits.x, limits.x),
    y: clamp(finiteNumber(transform?.y), -limits.y, limits.y),
  };
}

export function zoomLightboxTransform(transform, nextScale, metrics, anchor = {}) {
  const current = clampLightboxTransform(transform, metrics);
  const scale = clampLightboxScale(nextScale);
  const viewportWidth = positiveNumber(metrics?.viewportWidth);
  const viewportHeight = positiveNumber(metrics?.viewportHeight);
  const centerX = viewportWidth / 2;
  const centerY = viewportHeight / 2;
  const anchorX = finiteNumber(anchor?.x, centerX);
  const anchorY = finiteNumber(anchor?.y, centerY);

  const imageX = (anchorX - centerX - current.x) / current.scale;
  const imageY = (anchorY - centerY - current.y) / current.scale;
  return clampLightboxTransform({
    scale,
    x: anchorX - centerX - (imageX * scale),
    y: anchorY - centerY - (imageY * scale),
  }, metrics);
}

export function panLightboxTransform(transform, delta, metrics) {
  const current = clampLightboxTransform(transform, metrics);
  return clampLightboxTransform({
    ...current,
    x: current.x + finiteNumber(delta?.x),
    y: current.y + finiteNumber(delta?.y),
  }, metrics);
}

export function lightboxCanPan(transform, metrics) {
  const limits = lightboxPanLimits(transform, metrics);
  return limits.x > 0 || limits.y > 0;
}
