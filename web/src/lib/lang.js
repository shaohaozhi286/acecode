// 文件扩展名 → highlight.js 语言名映射,与 lib/markdown.js 注册的 12 种保持一致。
// SidePanel 「预览」tab 用这个把 .cpp / .ts 等映射到 hljs 知道的 lang token。
//
// 注册的 hljs 语言(在 markdown.js 里):
//   c, cpp, javascript, typescript, python, bash, json, diff, markdown, rust, go, yaml
// 任何不在此列表的扩展名 → '' (返回空字符串,前端用 escape-only 渲染,不上色)。

const EXT_TO_LANG = {
  c: 'c',
  h: 'c',
  cpp: 'cpp',
  cxx: 'cpp',
  cc: 'cpp',
  hpp: 'cpp',
  hxx: 'cpp',
  inl: 'cpp',
  js: 'javascript',
  jsx: 'javascript',
  mjs: 'javascript',
  cjs: 'javascript',
  ts: 'typescript',
  tsx: 'typescript',
  py: 'python',
  pyi: 'python',
  sh: 'bash',
  bash: 'bash',
  zsh: 'bash',
  json: 'json',
  jsonc: 'json',
  diff: 'diff',
  patch: 'diff',
  md: 'markdown',
  markdown: 'markdown',
  rs: 'rust',
  go: 'go',
  yaml: 'yaml',
  yml: 'yaml',
};

// 一些常见无后缀文件名 → lang(Dockerfile / Makefile 都没标准 hljs lang,
// 退到 bash 大致够用 — 真正的 dockerfile lang 没注册)。
const NAME_TO_LANG = {
  Dockerfile: 'bash',
  Makefile: 'bash',
  CMakeLists: '',
  '.gitignore': '',
  '.npmrc': '',
};

/**
 * @param {string} pathOrName 完整路径或仅文件名
 * @returns {string} 注册过的 hljs language id,或 '' 表示不上色
 */
export function langForFile(pathOrName) {
  if (!pathOrName) return '';
  const name = String(pathOrName).split(/[\\/]/).pop() || '';
  if (NAME_TO_LANG[name] !== undefined) return NAME_TO_LANG[name];
  // 取最后一个 . 之后的后缀,转小写
  const dot = name.lastIndexOf('.');
  if (dot < 0 || dot === name.length - 1) return '';
  const ext = name.slice(dot + 1).toLowerCase();
  return EXT_TO_LANG[ext] || '';
}
