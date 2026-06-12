// 控制台拖放文件 → 终端输入路径的纯函数(plan: 桌面控制台拖放文件 → 插入完整路径)。
//
// 职责:把 native 回传 / web drop 拿到的「file:// URI 或本地路径」字符串,先归一化
// 成本地文件系统路径,再按目标 OS 的 shell 规则做引用,拼成一段可直接粘进终端的
// 输入文本。三平台(windows / macos / linux)共用,Node 单测覆盖。
//
// 这里刻意不碰 DOM / xterm / WebSocket —— 保持纯函数,便于测试与跨端复用。
// 路径来源差异:
//   - Windows(WebView2 native 拦截):回传原始 file:// URI 字符串
//   - macOS(WKWebView swizzle):回传 NSURL.path,已是本地路径(无 file:// 前缀)
//   - Linux(WebKitGTK web drop):前端从 dataTransfer 的 text/uri-list 拿 file:// URI
// 因此本模块对「file:// URI」和「裸本地路径」两种输入都要兼容。

const WINDOWS = 'windows';

// file:// URI(或裸路径)→ 本地路径。
// 非 file:// 前缀的输入按「已是本地路径」原样返回(mac NSURL.path / 兜底)。
export function fileUriToLocalPath(input, os) {
  const raw = String(input == null ? '' : input).trim();
  if (!raw) return '';
  if (!/^file:\/\//i.test(raw)) return raw;

  // 去掉 file:// 前缀。标准格式:
  //   本地  file:///C:/a    → slice 后 "/C:/a"(host 为空,三斜杠)
  //   UNC   file://srv/share → slice 后 "srv/share"(带 host)
  let rest = raw.slice('file://'.length);
  // 解码 percent-encoding(%20 → 空格、中文等)。解码失败退回原串,不抛。
  try { rest = decodeURIComponent(rest); } catch { /* 保留 rest 原值 */ }

  if (os === WINDOWS) {
    if (rest.startsWith('/')) {
      // 本地盘符:/C:/a → C:/a → C:\a
      return rest.replace(/^\/+/, '').replace(/\//g, '\\');
    }
    // UNC:srv/share → \\srv\share
    return ('\\\\' + rest).replace(/\//g, '\\');
  }
  // posix:常见 file:///home/x → /home/x(保留前导斜杠)。
  // 万一带 host(file://localhost/home/x)则剥掉 host 段。
  return rest.startsWith('/') ? rest : rest.replace(/^[^/]*/, '');
}

// 单个本地路径 → shell 引用后的形式(需要时加引号)。
export function quoteShellPath(path, os) {
  const p = String(path == null ? '' : path);
  if (!p) return '';
  if (os === WINDOWS) {
    // cmd / powershell:含空格或命令元字符则用双引号包裹。Windows 没有单引号
    // 转义语义,双引号足以覆盖常见路径;路径内出现双引号极罕见,不额外处理。
    return /[\s&()<>^|"%!,;=]/.test(p) ? `"${p}"` : p;
  }
  // posix(macos / linux):白名单法,等价 python shlex.quote。
  // 仅由安全字符组成 → 原样;否则单引号包裹,内部单引号写成 '\'' 。
  if (/^[A-Za-z0-9_@%+=:,./-]+$/.test(p)) return p;
  return `'${p.replace(/'/g, `'\\''`)}'`;
}

// 多个「file:// URI 或本地路径」→ 一段终端输入文本:
// 各自归一化 + 引用,空格分隔,末尾补一个空格便于用户继续输入。空输入 → 空串。
export function formatDroppedPaths(items, os) {
  if (!Array.isArray(items)) return '';
  const parts = items
    .map((it) => fileUriToLocalPath(it, os))
    .filter((p) => p.length > 0)
    .map((p) => quoteShellPath(p, os));
  if (parts.length === 0) return '';
  return parts.join(' ') + ' ';
}

// 从 dataTransfer 的 text/uri-list 文本里解出 file:// URI 列表(Linux web-drop 路径)。
// text/uri-list 规范:每行一个 URI,以 # 开头的行是注释,需跳过。
export function parseUriList(text) {
  return String(text == null ? '' : text)
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.length > 0 && !line.startsWith('#'));
}
