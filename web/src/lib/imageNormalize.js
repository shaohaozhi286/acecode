export const IMAGE_NORMALIZE_THRESHOLD_BYTES = 10 * 1024 * 1024;
export const IMAGE_NORMALIZE_MAX_EDGE = 2560;
export const ATTACHMENT_HARD_LIMIT_BYTES = 25 * 1024 * 1024;

function isRasterImage(file) {
  const type = String(file?.type || '').toLowerCase();
  if (!type.startsWith('image/')) return false;
  return type !== 'image/svg+xml' && type !== 'image/gif';
}

function replaceExtension(name, mimeType) {
  const ext = mimeType === 'image/png' ? '.png'
    : mimeType === 'image/webp' ? '.webp'
      : '.jpg';
  const base = String(name || 'attachment').replace(/\.[^.\\/]+$/, '');
  return base + ext;
}

function canvasToBlob(canvas, type, quality) {
  return new Promise((resolve, reject) => {
    canvas.toBlob((blob) => {
      if (blob) resolve(blob);
      else reject(new Error('canvas encode failed'));
    }, type, quality);
  });
}

async function loadImageElement(file) {
  const url = URL.createObjectURL(file);
  try {
    const img = new Image();
    img.decoding = 'async';
    img.src = url;
    await img.decode();
    return img;
  } finally {
    URL.revokeObjectURL(url);
  }
}

async function decodeImage(file) {
  if (typeof createImageBitmap === 'function') {
    try {
      const bitmap = await createImageBitmap(file);
      return {
        width: bitmap.width,
        height: bitmap.height,
        source: bitmap,
        close: () => bitmap.close?.(),
        backend: 'createImageBitmap',
      };
    } catch (error) {
      console.warn('[image-normalize] createImageBitmap failed, falling back to Image', error);
    }
  }

  const img = await loadImageElement(file);
  return {
    width: img.naturalWidth || img.width,
    height: img.naturalHeight || img.height,
    source: img,
    close: () => {},
    backend: 'html-image',
  };
}

function hasCanvasAlpha(ctx, width, height) {
  try {
    const data = ctx.getImageData(0, 0, width, height).data;
    for (let i = 3; i < data.length; i += 4) {
      if (data[i] !== 255) return true;
    }
  } catch {
    return false;
  }
  return false;
}

export function shouldNormalizeImage({ size = 0, width = 0, height = 0 } = {}, {
  thresholdBytes = IMAGE_NORMALIZE_THRESHOLD_BYTES,
  maxEdge = IMAGE_NORMALIZE_MAX_EDGE,
} = {}) {
  return size >= thresholdBytes || Math.max(width, height) > maxEdge;
}

export async function normalizeImageFile(file, {
  thresholdBytes = IMAGE_NORMALIZE_THRESHOLD_BYTES,
  maxEdge = IMAGE_NORMALIZE_MAX_EDGE,
  jpegQuality = 0.85,
} = {}) {
  if (!isRasterImage(file)) {
    return { file, changed: false, backend: 'native', reason: 'not-raster-image' };
  }

  let decoded = null;
  try {
    decoded = await decodeImage(file);
    if (!shouldNormalizeImage({ size: file.size, width: decoded.width, height: decoded.height }, {
      thresholdBytes,
      maxEdge,
    })) {
      return {
        file,
        changed: false,
        backend: decoded.backend,
        reason: 'below-thresholds',
        width: decoded.width,
        height: decoded.height,
      };
    }

    const scale = Math.min(1, maxEdge / Math.max(decoded.width, decoded.height));
    const width = Math.max(1, Math.round(decoded.width * scale));
    const height = Math.max(1, Math.round(decoded.height * scale));
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d', { alpha: true });
    if (!ctx) throw new Error('2d canvas unavailable');
    ctx.drawImage(decoded.source, 0, 0, width, height);

    const hasAlpha = hasCanvasAlpha(ctx, width, height);
    const outputType = hasAlpha ? 'image/png' : 'image/jpeg';
    const qualities = outputType === 'image/jpeg'
      ? [jpegQuality, 0.8, 0.75, 0.7]
      : [undefined];
    let best = null;
    for (const quality of qualities) {
      const blob = await canvasToBlob(canvas, outputType, quality);
      if (!best || blob.size < best.size) best = blob;
      if (blob.size <= thresholdBytes) {
        best = blob;
        break;
      }
    }
    if (!best || best.size >= file.size) {
      console.info('[image-normalize] native kept original', {
        name: file.name,
        originalSize: file.size,
        encodedSize: best?.size,
        width: decoded.width,
        height: decoded.height,
      });
      return {
        file,
        changed: false,
        backend: decoded.backend,
        reason: 'encoded-not-smaller',
        width: decoded.width,
        height: decoded.height,
      };
    }

    const normalized = new File([best], replaceExtension(file.name, outputType), {
      type: outputType,
      lastModified: file.lastModified || Date.now(),
    });
    console.info('[image-normalize] native changed image', {
      name: file.name,
      outputName: normalized.name,
      originalSize: file.size,
      outputSize: normalized.size,
      originalType: file.type,
      outputType,
      width: decoded.width,
      height: decoded.height,
      outputWidth: width,
      outputHeight: height,
      backend: decoded.backend,
    });
    return {
      file: normalized,
      changed: true,
      backend: decoded.backend,
      reason: file.size > thresholdBytes ? 'size-threshold' : 'dimension-threshold',
      originalFile: file,
      width: decoded.width,
      height: decoded.height,
      outputWidth: width,
      outputHeight: height,
    };
  } catch (error) {
    console.warn('[image-normalize] native normalization failed; daemon stb fallback will handle it', {
      name: file?.name,
      size: file?.size,
      type: file?.type,
      error: error?.message || String(error),
    });
    return { file, changed: false, backend: 'native-failed', reason: 'fallback-to-daemon-stb', error };
  } finally {
    decoded?.close?.();
  }
}
