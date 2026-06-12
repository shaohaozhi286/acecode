// consoleDropPaths.js 单测(plan: 桌面控制台拖放文件 → 插入完整路径)。
// 覆盖:file:// URI → 本地路径(win/posix、percent 解码、UNC)、shell 引用规则
// (含空格 / 元字符 / 单引号转义)、多文件拼接与尾随空格、uri-list 解析。

import assert from 'node:assert/strict';
import {
  fileUriToLocalPath,
  quoteShellPath,
  formatDroppedPaths,
  parseUriList,
} from './consoleDropPaths.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 触发场景:Windows 下拖来一个带空格的文件,native 回传 file:/// URI。
// 期望:percent 解码 + 去三斜杠前导 + 反斜杠分隔,得到盘符路径。
run('fileUriToLocalPath: windows file URI with %20 → backslash path', () => {
  assert.equal(
    fileUriToLocalPath('file:///C:/Users/a%20b/note.txt', 'windows'),
    'C:\\Users\\a b\\note.txt');
});

// 触发场景:Windows UNC 网络路径 file://server/share/x。
// 期望:还原成 \\server\share\x(双反斜杠开头)。
run('fileUriToLocalPath: windows UNC host → \\\\server\\share', () => {
  assert.equal(
    fileUriToLocalPath('file://server/share/x.txt', 'windows'),
    '\\\\server\\share\\x.txt');
});

// 触发场景:Linux 从文件管理器拖来,uri-list 给 file:// URI。
// 期望:posix 保留前导斜杠,percent 解码空格。
run('fileUriToLocalPath: posix file URI keeps leading slash', () => {
  assert.equal(
    fileUriToLocalPath('file:///home/u/a%20b.txt', 'linux'),
    '/home/u/a b.txt');
});

// 触发场景:macOS swizzle 回传的是 NSURL.path(已是裸本地路径,无 file:// 前缀)。
// 期望:原样返回,不被当作 URI 处理。
run('fileUriToLocalPath: bare local path returned as-is', () => {
  assert.equal(fileUriToLocalPath('/Users/u/a b.txt', 'macos'), '/Users/u/a b.txt');
  assert.equal(fileUriToLocalPath('C:\\x\\y.txt', 'windows'), 'C:\\x\\y.txt');
});

// 触发场景:解码非 ASCII(中文)文件名。期望:还原成可读字符。
run('fileUriToLocalPath: decodes non-ascii percent-encoding', () => {
  assert.equal(
    fileUriToLocalPath('file:///home/u/%E6%96%87%E6%A1%A3.txt', 'linux'),
    '/home/u/文档.txt');
});

// 触发场景:空 / null 输入。期望:返回空串,不抛。
run('fileUriToLocalPath: empty input → empty string', () => {
  assert.equal(fileUriToLocalPath('', 'windows'), '');
  assert.equal(fileUriToLocalPath(null, 'linux'), '');
});

// 触发场景:Windows 路径含空格。期望:双引号包裹;无空格则不加引号。
run('quoteShellPath: windows quotes only when needed', () => {
  assert.equal(quoteShellPath('C:\\a b\\f.txt', 'windows'), '"C:\\a b\\f.txt"');
  assert.equal(quoteShellPath('C:\\ab\\f.txt', 'windows'), 'C:\\ab\\f.txt');
});

// 触发场景:Windows 路径含命令元字符 &(无引号会被 cmd 当命令分隔)。
// 期望:即使没空格也要加双引号。回归:裸 & 会截断命令。
run('quoteShellPath: windows quotes path containing ampersand', () => {
  assert.equal(quoteShellPath('C:\\a&b\\f.txt', 'windows'), '"C:\\a&b\\f.txt"');
});

// 触发场景:posix 普通安全路径 vs 含空格路径。
// 期望:安全字符集原样;含空格用单引号包裹(等价 shlex.quote)。
run('quoteShellPath: posix safe path bare, spaced path single-quoted', () => {
  assert.equal(quoteShellPath('/home/u/file.txt', 'linux'), '/home/u/file.txt');
  assert.equal(quoteShellPath('/home/u/a b.txt', 'linux'), `'/home/u/a b.txt'`);
});

// 触发场景:posix 路径里本身含单引号。期望:单引号用 '\'' 序列转义,
// 避免破坏外层引用。回归:未转义会让 shell 引用提前闭合。
run('quoteShellPath: posix escapes embedded single quote', () => {
  assert.equal(quoteShellPath(`/home/u/it's.txt`, 'macos'), `'/home/u/it'\\''s.txt'`);
});

// 触发场景:一次拖入多个文件(含空格)。期望:各自引用、空格分隔、末尾补一个空格。
run('formatDroppedPaths: multiple files joined with trailing space', () => {
  const out = formatDroppedPaths(
    ['file:///home/u/a.txt', 'file:///home/u/b%20c.txt'], 'linux');
  assert.equal(out, `/home/u/a.txt '/home/u/b c.txt' `);
});

// 触发场景:数组里混有 file:// URI 与裸本地路径(mac 实际形态)。
// 期望:两种都被正确归一化与引用。
run('formatDroppedPaths: mixes file URI and bare path', () => {
  const out = formatDroppedPaths(['/Users/u/a b.txt', 'file:///Users/u/c.txt'], 'macos');
  assert.equal(out, `'/Users/u/a b.txt' /Users/u/c.txt `);
});

// 触发场景:空数组 / 非数组。期望:返回空串(不注入任何东西)。
run('formatDroppedPaths: empty or non-array → empty string', () => {
  assert.equal(formatDroppedPaths([], 'windows'), '');
  assert.equal(formatDroppedPaths(null, 'windows'), '');
});

// 触发场景:解析 text/uri-list 文本(含注释行 / 空行 / CRLF)。
// 期望:跳过 # 注释与空行,trim 后保留 URI 顺序。
run('parseUriList: skips comments and blank lines', () => {
  const text = '# comment\r\nfile:///a.txt\r\n\r\nfile:///b%20c.txt\r\n';
  assert.deepEqual(parseUriList(text), ['file:///a.txt', 'file:///b%20c.txt']);
});
