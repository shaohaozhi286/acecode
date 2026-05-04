// markdown → safe HTML 渲染管线。
// 由 markdown-it 14 + highlight.js core(12 种常用语言)驱动:
//   - GFM 表格 / 任务清单 / 嵌套 list / autolink
//   - 代码高亮(c/cpp/js/ts/python/bash/json/diff/markdown/rust/go/yaml)
//   - HTML 解析关闭(html: false),URL scheme 白名单(http/https/mailto/相对)
// 公开 API: renderMarkdown(src) -> string,签名跟旧版一致,Message.jsx 零改。

import MarkdownIt from 'markdown-it';
import taskLists from 'markdown-it-task-lists';

import hljs from 'highlight.js/lib/core';
import c          from 'highlight.js/lib/languages/c';
import cpp        from 'highlight.js/lib/languages/cpp';
import javascript from 'highlight.js/lib/languages/javascript';
import typescript from 'highlight.js/lib/languages/typescript';
import python     from 'highlight.js/lib/languages/python';
import bash       from 'highlight.js/lib/languages/bash';
import json       from 'highlight.js/lib/languages/json';
import diff       from 'highlight.js/lib/languages/diff';
import markdown   from 'highlight.js/lib/languages/markdown';
import rust       from 'highlight.js/lib/languages/rust';
import go         from 'highlight.js/lib/languages/go';
import yaml       from 'highlight.js/lib/languages/yaml';

hljs.registerLanguage('c',          c);
hljs.registerLanguage('cpp',        cpp);
hljs.registerLanguage('javascript', javascript);
hljs.registerLanguage('typescript', typescript);
hljs.registerLanguage('python',     python);
hljs.registerLanguage('bash',       bash);
hljs.registerLanguage('json',       json);
hljs.registerLanguage('diff',       diff);
hljs.registerLanguage('markdown',   markdown);
hljs.registerLanguage('rust',       rust);
hljs.registerLanguage('go',         go);
hljs.registerLanguage('yaml',       yaml);

// 常见别名一并归一(markdown-it 用 fence info 第一个 token 当 lang)
const LANG_ALIAS = {
  js: 'javascript',
  jsx: 'javascript',
  ts: 'typescript',
  tsx: 'typescript',
  py: 'python',
  sh: 'bash',
  shell: 'bash',
  zsh: 'bash',
  md: 'markdown',
  yml: 'yaml',
  'c++': 'cpp',
  cxx: 'cpp',
  cc: 'cpp',
  rs: 'rust',
  golang: 'go',
};

function normalizeLang(lang) {
  if (!lang) return '';
  const l = String(lang).trim().toLowerCase();
  return LANG_ALIAS[l] || l;
}

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function highlightCode(str, lang) {
  const norm = normalizeLang(lang);
  if (norm && hljs.getLanguage(norm)) {
    try {
      return {
        lang: norm,
        html: hljs.highlight(str, { language: norm, ignoreIllegals: true }).value,
      };
    } catch { /* fall through to plain */ }
  }
  return { lang: norm && hljs.getLanguage(norm) ? norm : '', html: escapeHtml(str) };
}

const md = new MarkdownIt({
  html: false,        // 禁 raw HTML(XSS 防御)
  linkify: true,      // 自动链接化裸 URL
  breaks: false,      // 单换行不变 <br>(GFM 行为可选,我们走标准)
  typographer: false, // 不做 smart quotes
  highlight(str, lang) {
    const highlighted = highlightCode(str, lang);
    if (highlighted.lang) {
      return `<pre class="hljs"><code class="hljs language-${escapeHtml(highlighted.lang)}">${highlighted.html}</code></pre>`;
    }
    return `<pre><code>${highlighted.html}</code></pre>`;
  },
});

md.use(taskLists, { enabled: false, label: false });

md.renderer.rules.fence = (tokens, idx) => {
  const token = tokens[idx];
  const info = String(token.info || '').trim();
  const lang = info ? info.split(/\s+/g)[0] : '';
  const highlighted = highlightCode(token.content, lang);
  const preClass = highlighted.lang ? ' class="hljs"' : '';
  const codeClass = highlighted.lang ? ` class="hljs language-${escapeHtml(highlighted.lang)}"` : '';
  const langAttr = highlighted.lang ? ` data-code-lang="${escapeHtml(highlighted.lang)}"` : '';
  return `<div class="ace-copyable-code" data-code-copy-frame="true"${langAttr}>`
    + `<button type="button" class="ace-code-copy-btn" data-code-copy-button="true" title="复制代码" aria-label="复制代码"></button>`
    + `<pre${preClass}><code${codeClass} data-code-copy-source="true">${highlighted.html}</code></pre>`
    + `</div>\n`;
};

// URL scheme 白名单。markdown-it 的 validateLink 默认放过 javascript: + data:,
// 我们收紧:只允许 http/https/mailto 或相对路径(/, ./, ../, #)。
md.validateLink = (url) => {
  const t = String(url).trim();
  if (!t) return false;
  return /^(https?:|mailto:|\/|\.|#)/i.test(t);
};

// link 默认 target=_blank rel=noreferrer(避免 referer 泄漏)
const defaultLinkOpen = md.renderer.rules.link_open
  || ((tokens, idx, options, _env, self) => self.renderToken(tokens, idx, options));
md.renderer.rules.link_open = (tokens, idx, options, env, self) => {
  const token = tokens[idx];
  // 只有外链(http/https)才加 target=_blank;相对链接(#anchor)保持当前页
  const href = token.attrGet('href') || '';
  if (/^https?:/i.test(href)) {
    token.attrSet('target', '_blank');
    token.attrSet('rel', 'noreferrer');
  }
  return defaultLinkOpen(tokens, idx, options, env, self);
};

export function renderMarkdown(src) {
  if (!src) return '';
  try {
    return md.render(String(src));
  } catch {
    // 任何 markdown-it 内部异常 → 退化到 escape-only,绝不返回未转义 HTML
    return `<p>${escapeHtml(String(src))}</p>`;
  }
}
