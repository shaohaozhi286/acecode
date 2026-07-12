import assert from 'node:assert/strict';
import { classifyFileLink, splitLineSuffix } from './fileLink.js';
import { renderMarkdown } from './markdown.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:工作区相对路径(模型被 prompt 要求输出的主力形态)。
// 期望:判为 file,path 原样,无行号。这是「点击开详情页预览」的核心通路。
await run('relative workspace path classifies as file', () => {
  const r = classifyFileLink('src/prompt/system_prompt.cpp');
  assert.equal(r.kind, 'file');
  assert.equal(r.path, 'src/prompt/system_prompt.cpp');
  assert.equal(r.line, null);
});

// 场景:相对路径带 :行号(claude-code 风格 foo.cpp:130)。
// 期望:剥出 line=130,path 不含行号 —— 行号不能混进要读文件的 path。
await run('relative path with line suffix splits path and line', () => {
  const r = classifyFileLink('src/prompt/system_prompt.cpp:130');
  assert.equal(r.kind, 'file');
  assert.equal(r.path, 'src/prompt/system_prompt.cpp');
  assert.equal(r.line, 130);
});

// 场景:./ 和 ../ 开头的相对路径。
// 期望:同样判 file(旧 validateLink 白名单只放 `.` 开头,这里保持兼容)。
await run('dot-relative paths classify as file', () => {
  assert.equal(classifyFileLink('./a/b.js').kind, 'file');
  assert.equal(classifyFileLink('../a/b.js').kind, 'file');
});

// 场景:Windows 盘符绝对路径(用户选了「工作区内 + 绝对路径」,模型偶尔吐全路径)。
// 期望:盘符里的冒号不被当 URL scheme,判 file;正/反斜杠都认;结尾 :行号照剥。
await run('windows absolute path classifies as file (both slash styles)', () => {
  assert.equal(classifyFileLink('N:\\Users\\shao\\acecode\\src\\a.cpp').kind, 'file');
  assert.equal(classifyFileLink('N:/Users/shao/acecode/src/a.cpp').kind, 'file');
  const r = classifyFileLink('N:\\Users\\shao\\acecode\\src\\a.cpp:42');
  assert.equal(r.kind, 'file');
  assert.equal(r.path, 'N:\\Users\\shao\\acecode\\src\\a.cpp');
  assert.equal(r.line, 42);
});

// 场景:POSIX 绝对路径。
// 期望:判 file(旧白名单已放 `/` 开头,行为不回退)。
await run('posix absolute path classifies as file', () => {
  assert.equal(classifyFileLink('/home/user/x.py').kind, 'file');
});

// 场景:真外链 http/https/mailto。
// 期望:判 external(交给 link_open 加 target=_blank,不当文件预览)。
await run('http/https/mailto classify as external', () => {
  assert.equal(classifyFileLink('https://example.com/a').kind, 'external');
  assert.equal(classifyFileLink('http://example.com').kind, 'external');
  assert.equal(classifyFileLink('mailto:a@b.com').kind, 'external');
});

// 场景:协议相对 URL //host/path。
// 期望:判 external,而不是被误当成 POSIX 绝对文件路径。
await run('protocol-relative url classifies as external', () => {
  assert.equal(classifyFileLink('//cdn.example.com/x.js').kind, 'external');
});

// 场景:页内锚点。
// 期望:判 anchor,保持当前页滚动语义,不拦截、不预览。
await run('hash anchor classifies as anchor', () => {
  assert.equal(classifyFileLink('#section').kind, 'anchor');
});

// 回归:XSS 向量 javascript:/data:/vbscript: 必须被拒。
// 这是 validateLink 存在的原因 —— 放行相对路径时绝不能顺带放行危险 scheme。
await run('dangerous schemes are rejected', () => {
  assert.equal(classifyFileLink('javascript:alert(1)').kind, 'reject');
  assert.equal(classifyFileLink('data:text/html,<script>').kind, 'reject');
  assert.equal(classifyFileLink('vbscript:msgbox').kind, 'reject');
});

// 边界:空串。
// 期望:reject,避免生成空 data-file-path 的死链接。
await run('empty href is rejected', () => {
  assert.equal(classifyFileLink('').kind, 'reject');
  assert.equal(classifyFileLink('   ').kind, 'reject');
});

// splitLineSuffix 单元:只认末尾纯数字冒号,盘符/中段冒号不动。
await run('splitLineSuffix only strips trailing numeric colon', () => {
  assert.deepEqual(splitLineSuffix('a/b.c:12'), { path: 'a/b.c', line: 12 });
  assert.deepEqual(splitLineSuffix('a/b.c:12:5'), { path: 'a/b.c', line: 12 });
  assert.deepEqual(splitLineSuffix('a/b.c'), { path: 'a/b.c', line: null });
  assert.deepEqual(splitLineSuffix('N:\\x\\y.c'), { path: 'N:\\x\\y.c', line: null });
});

// 集成:renderMarkdown 把相对路径链接渲染成带 data-file-path 的可点锚点。
// 回归 bug:旧 validateLink 白名单 /^(https?:|mailto:|\/|\.|#)/ 会把裸相对路径 `docs/foo.md`
// 判非法 → markdown-it 剥成纯文本(连 <a> 都没有),表现为「有文件名没链接」。
await run('markdown renders relative file link with data-file-path', () => {
  const html = renderMarkdown('see [design](docs/spec.md:10) here');
  assert.match(html, /<a[^>]*data-file-path="docs\/spec\.md"/);
  assert.match(html, /data-file-line="10"/);
  assert.match(html, /class="[^"]*ace-file-link/);
  // 文件链接不应带 target=_blank(不走浏览器新标签页,走详情页预览)。
  assert.doesNotMatch(html, /<a[^>]*data-file-path[^>]*target="_blank"/);
});

// 集成:外链仍旧 target=_blank rel=noreferrer,且不带 data-file-path。
await run('markdown keeps external links as new-tab, not file', () => {
  const html = renderMarkdown('[site](https://example.com)');
  assert.match(html, /target="_blank"/);
  assert.match(html, /rel="noreferrer"/);
  assert.doesNotMatch(html, /data-file-path/);
});

// 集成:javascript: 链接被 validateLink 拒 → markdown-it 不生成 <a>,退化为纯文本。
await run('markdown drops javascript: link to plain text', () => {
  const html = renderMarkdown('[x](javascript:alert(1))');
  assert.doesNotMatch(html, /<a[^>]*href="javascript:/i);
});

console.log('all fileLink tests passed');
