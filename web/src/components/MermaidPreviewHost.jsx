import { useCallback, useEffect, useRef, useState } from 'react';
import {
  MERMAID_PREVIEW_EVENT,
  mermaidPreviewCanvasColor,
  normalizeMermaidPreviewDetail,
} from '../lib/mermaidPreview.js';
import { ImageLightbox } from './ImageLightbox.jsx';

export function MermaidPreviewHost() {
  const activeUrlRef = useRef('');
  const [preview, setPreview] = useState(null);

  const revoke = useCallback((url) => {
    if (!url) return;
    try {
      window.URL.revokeObjectURL(url);
    } catch {
      // Best effort during document teardown.
    }
  }, []);

  const closePreview = useCallback(() => {
    const activeUrl = activeUrlRef.current;
    activeUrlRef.current = '';
    setPreview(null);
    revoke(activeUrl);
  }, [revoke]);

  useEffect(() => {
    const openPreview = (event) => {
      const detail = normalizeMermaidPreviewDetail(event.detail);
      if (!detail) return;
      let objectUrl = '';
      try {
        objectUrl = window.URL.createObjectURL(new window.Blob(
          [detail.svg],
          { type: 'image/svg+xml;charset=utf-8' },
        ));
      } catch {
        return;
      }

      const previousUrl = activeUrlRef.current;
      activeUrlRef.current = objectUrl;
      setPreview({
        src: objectUrl,
        alt: detail.alt,
        width: detail.width,
        height: detail.height,
        canvasColor: mermaidPreviewCanvasColor(detail.theme),
        mermaidExport: detail,
      });
      revoke(previousUrl);
    };

    window.addEventListener(MERMAID_PREVIEW_EVENT, openPreview);
    return () => {
      window.removeEventListener(MERMAID_PREVIEW_EVENT, openPreview);
      const activeUrl = activeUrlRef.current;
      activeUrlRef.current = '';
      revoke(activeUrl);
    };
  }, [revoke]);

  return <ImageLightbox preview={preview} onClose={closePreview} />;
}
