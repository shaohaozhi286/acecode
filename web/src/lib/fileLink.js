// 判定 markdown 链接的语义:外链 / 页内锚点 / 本地文件 / 应拒绝。
//
// 单一事实源:markdown.js 的 validateLink + link_open,与 Message.jsx 的点击拦截
// 都调这里。让「一个链接是否被渲染成可点文件链接」与「点击后是否走详情页预览」
// 用同一套判据 —— 否则很容易出现「渲染成蓝链但点了没反应」或「点了跳到错误 URL」。
//
// 安全边界:validateLink 原本靠白名单挡 javascript:/data:。这里放行相对本地路径的
// 同时,任何带 URL scheme(且非 http/https/mailto)的 href 一律判 reject,保留原有
// 防护。POSIX 绝对路径(/home/...)与 Windows 盘符绝对路径(N:\ 或 N:/)都算文件。

// 只认「结尾紧跟数字」的冒号作为 :行号(:12 或 :12:34,后者取首个数字为行)。
// 锚定到末尾 + 要求纯数字,避免误伤 Windows 盘符(N:\)或 URL scheme(http:)。
export function splitLineSuffix(raw) {
  const s = String(raw == null ? '' : raw);
  const m = /:(\d+)(?::\d+)?$/.exec(s);
  if (!m) return { path: s, line: null };
  return { path: s.slice(0, m.index), line: Number(m[1]) };
}

const WIN_ABS = /^[A-Za-z]:[\\/]/;         // N:\Users\... 或 N:/Users/...
const URL_SCHEME = /^[a-z][a-z0-9+.-]*:/i; // http: mailto: javascript: data: tel: ...

// 返回 { kind, path, line }。kind ∈ 'external' | 'anchor' | 'file' | 'reject'。
// path/line 仅在 kind==='file' 有意义。
export function classifyFileLink(rawHref) {
  const href = String(rawHref == null ? '' : rawHref).trim();
  if (!href) return { kind: 'reject', path: '', line: null };
  // 页内锚点保持原样(当前页滚动)。
  if (href[0] === '#') return { kind: 'anchor', path: '', line: null };
  // 协议相对 URL(//host/path)当外链,避免被当成 POSIX 绝对路径。
  if (href.startsWith('//')) return { kind: 'external', path: '', line: null };
  // 真外链:新标签页打开(交给 link_open 加 target=_blank)。
  if (/^https?:/i.test(href)) return { kind: 'external', path: '', line: null };
  if (/^mailto:/i.test(href)) return { kind: 'external', path: '', line: null };

  // Windows 盘符绝对路径:先判文件(盘符里的冒号不能当 scheme),再剥 :行号。
  if (WIN_ABS.test(href)) {
    const { path, line } = splitLineSuffix(href);
    return { kind: 'file', path, line };
  }

  // 其余:先剥 :行号,再看剩下的是否带 scheme。带 scheme(javascript:/data:/...)一律拒绝。
  // 先剥行号是为了不把 `foo.md:42` 里的 `foo.md:` 误判成 scheme。
  const { path, line } = splitLineSuffix(href);
  if (URL_SCHEME.test(path)) return { kind: 'reject', path: '', line: null };
  // 无 scheme 的相对路径(docs/foo.md、./x、../y)或 POSIX 绝对路径(/home/...)→ 本地文件。
  return { kind: 'file', path, line };
}
