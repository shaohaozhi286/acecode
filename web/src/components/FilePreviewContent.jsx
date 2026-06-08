import { useEffect, useState } from 'react';
import hljs from 'highlight.js/lib/core';
import { ApiError } from '../lib/api.js';
import { langForFile } from '../lib/lang.js';
import { renderMarkdown } from '../lib/markdown.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import { resolveSelectionSourcePath } from '../lib/selectionChatContext.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { clsx, formatBytes } from '../lib/format.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const MARKDOWN_EXTENSIONS = new Set(['md', 'markdown']);
const IMAGE_EXTENSIONS = new Set(['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'ico', 'svg']);

function extensionForPath(path) {
  const name = String(path || '').split(/[\\/]/).pop() || '';
  const dot = name.lastIndexOf('.');
  if (dot < 0 || dot === name.length - 1) return '';
  return name.slice(dot + 1).toLowerCase();
}

function isMarkdownPreview(path) {
  return MARKDOWN_EXTENSIONS.has(extensionForPath(path));
}

function isImagePreview(path) {
  return IMAGE_EXTENSIONS.has(extensionForPath(path));
}

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

async function copyWithToast(text, okText) {
  const result = await copyTextToSystemClipboard(text);
  if (result.ok) toast({ kind: 'ok', text: okText });
  else toast({ kind: 'err', text: '复制失败:' + (result.error || '') });
}

export function FilePreviewContent({ api, cwd, path, wrapPreview, onToggleWrapPreview }) {
  const [state, setState] = useState({
    status: 'idle',
    kind: 'text',
    text: '',
    error: null,
    lang: '',
    size: 0,
    imageUrl: '',
    contentType: '',
  });
  const [markdownSource, setMarkdownSource] = useState(false);
  const [imagePreview, setImagePreview] = useState(null);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'preview' || target.path !== path) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_TEXT) {
        detail.handled = true;
        if (!state.text) {
          toast({ kind: 'info', text: '没有可复制的预览文本' });
          return;
        }
        copyWithToast(state.text, '已复制预览内容');
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_METADATA) {
        detail.handled = true;
        const metadata = [
          `path: ${path}`,
          `kind: ${state.kind}`,
          `size: ${state.size}`,
          state.contentType ? `content-type: ${state.contentType}` : '',
          state.lang ? `language: ${state.lang}` : '',
        ].filter(Boolean).join('\n');
        copyWithToast(metadata, '已复制预览信息');
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [path, state.contentType, state.kind, state.lang, state.size, state.text]);

  useEffect(() => {
    if (!cwd || !path) {
      setState({ status: 'idle', kind: 'text', text: '', error: null, lang: '', size: 0, imageUrl: '', contentType: '' });
      return undefined;
    }
    let cancelled = false;
    let objectUrl = '';
    setMarkdownSource(false);
    setImagePreview(null);
    setState({ status: 'loading', kind: 'text', text: '', error: null, lang: '', size: 0, imageUrl: '', contentType: '' });

    if (isImagePreview(path)) {
      api.readFileBlob(cwd, path).then((blob) => {
        if (cancelled) return;
        objectUrl = URL.createObjectURL(blob);
        setState({
          status: 'ok',
          kind: 'image',
          text: '',
          error: null,
          lang: '',
          size: blob.size || 0,
          imageUrl: objectUrl,
          contentType: blob.type || '',
        });
      }).catch((err) => {
        if (cancelled) return;
        let msg = '读取失败';
        let extraSize = 0;
        if (err instanceof ApiError) {
          const body = err.body;
          if (body && typeof body === 'object') {
            if (body.error === 'file too large') {
              extraSize = Number(body.size || 0);
              msg = `文件过大 (${formatBytes(extraSize)}),无法在浏览器内预览`;
            } else if (body.error === 'unsupported file type') msg = '该图片格式暂不支持预览';
            else if (body.error === 'not found') msg = '文件不存在';
            else if (body.error) msg = body.error;
          } else {
            msg = `读取失败 (HTTP ${err.status})`;
          }
        }
        setState({ status: 'error', kind: 'image', text: '', error: msg, lang: '', size: extraSize, imageUrl: '', contentType: '' });
      });
      return () => {
        cancelled = true;
        if (objectUrl) URL.revokeObjectURL(objectUrl);
      };
    }

    api.readFile(cwd, path).then((text) => {
      if (cancelled) return;
      setState({
        status: 'ok',
        kind: isMarkdownPreview(path) ? 'markdown' : 'text',
        text,
        error: null,
        lang: langForFile(path),
        size: text.length,
        imageUrl: '',
        contentType: '',
      });
    }).catch((err) => {
      if (cancelled) return;
      let msg = '读取失败';
      let extraSize = 0;
      if (err instanceof ApiError) {
        const body = err.body;
        if (body && typeof body === 'object') {
          if (body.error === 'binary') msg = '二进制文件,无法预览';
          else if (body.error === 'file too large') {
            extraSize = Number(body.size || 0);
            msg = `文件过大 (${formatBytes(extraSize)}),无法在浏览器内预览`;
          } else if (body.error === 'not found') msg = '文件不存在';
          else if (body.error) msg = body.error;
        } else {
          msg = `读取失败 (HTTP ${err.status})`;
        }
      }
      setState({ status: 'error', kind: 'text', text: '', error: msg, lang: '', size: extraSize, imageUrl: '', contentType: '' });
    });
    return () => { cancelled = true; };
  }, [api, cwd, path]);

  if (!path) {
    return <div className="ace-empty-state">未选中文件,请在「文件」中点击一个文件</div>;
  }
  if (state.status === 'loading') {
    return <div className="ace-empty-state">加载中...</div>;
  }
  if (state.status === 'error') {
    return (
      <div className="ace-empty-state">
        <div className="text-danger text-[12px] mb-1">{state.error}</div>
        <div className="text-fg-mute text-[10px] font-mono opacity-70 break-all">{path}</div>
      </div>
    );
  }
  if (state.status !== 'ok') return null;
  const sourcePath = resolveSelectionSourcePath({ cwd, path });
  const previewAttrs = {
    'data-desktop-preview-path': path || undefined,
    'data-desktop-preview-cwd': cwd || undefined,
    'data-desktop-preview-source-path': sourcePath || path || undefined,
    'data-desktop-preview-kind': state.kind || undefined,
    'data-desktop-preview-size': Number.isFinite(state.size) ? String(state.size) : undefined,
    'data-desktop-preview-content-type': state.contentType || undefined,
  };
  if (state.kind === 'image') {
    return (
      <div className="flex-1 flex flex-col overflow-hidden" {...previewAttrs}>
        <button
          type="button"
          className="ace-side-image-preview"
          title="预览图片"
          aria-label="预览图片"
          onClick={() => setImagePreview({ src: state.imageUrl, alt: path })}
        >
          <img src={state.imageUrl} alt={path} draggable="false" />
        </button>
        <ImageLightbox preview={imagePreview} onClose={() => setImagePreview(null)} />
      </div>
    );
  }

  const lang = state.lang;
  let html;
  if (lang && hljs.getLanguage(lang)) {
    try {
      html = `<pre class="hljs"><code class="hljs language-${escapeHtml(lang)}">`
           + hljs.highlight(state.text, { language: lang, ignoreIllegals: true }).value
           + `</code></pre>`;
    } catch {
      html = `<pre><code>${escapeHtml(state.text)}</code></pre>`;
    }
  } else {
    html = `<pre><code>${escapeHtml(state.text)}</code></pre>`;
  }
  const wrapTitle = wrapPreview ? '关闭自动换行' : '开启自动换行';
  const isMarkdown = state.kind === 'markdown';
  const showMarkdownRendered = isMarkdown && !markdownSource;
  const markdownToggleTitle = markdownSource ? '渲染 Markdown' : '查看 Markdown 原文';
  return (
    <div className="flex-1 flex flex-col overflow-hidden" {...previewAttrs}>
      <CopyableCodeFrame
        text={state.text}
        className="flex-1 min-h-0 ace-side-preview-code"
        data-wrap={wrapPreview ? 'true' : 'false'}
        actions={(
          <>
            {isMarkdown && (
              <button
                type="button"
                className={clsx('ace-code-action-btn ace-code-markdown-btn', showMarkdownRendered && 'is-active')}
                title={markdownToggleTitle}
                aria-label={markdownToggleTitle}
                aria-pressed={showMarkdownRendered}
                onClick={(event) => { event.stopPropagation(); setMarkdownSource((prev) => !prev); }}
              >
                <VsIcon name={markdownSource ? 'document' : 'code'} size={14} />
              </button>
            )}
            {!showMarkdownRendered && (
              <button
                type="button"
                className={clsx('ace-code-action-btn ace-code-wrap-btn', wrapPreview && 'is-active')}
                title={wrapTitle}
                aria-label={wrapTitle}
                aria-pressed={wrapPreview}
                onClick={(event) => { event.stopPropagation(); onToggleWrapPreview?.(); }}
              >
                <VsIcon name="wordWrap" size={14} />
              </button>
            )}
          </>
        )}
      >
        {showMarkdownRendered ? (
          <div
            className="h-full overflow-auto ace-md ace-side-markdown-preview"
            dangerouslySetInnerHTML={{ __html: renderMarkdown(state.text) }}
          />
        ) : (
          <div
            className="h-full overflow-auto text-[11px] ace-preview"
            dangerouslySetInnerHTML={{ __html: html }}
          />
        )}
      </CopyableCodeFrame>
    </div>
  );
}
