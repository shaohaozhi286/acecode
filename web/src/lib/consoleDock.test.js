// consoleDock.js 单测(openspec/changes/add-console-dock 任务 4.2)。
// 覆盖:tab 状态机的增删切换与 exited 标记、WS 帧分流(0x00 控制帧 vs 数据)、
// 重连退避节奏、dock 高度 clamp、WS URL 构建。

import assert from 'node:assert/strict';
import {
  CONSOLE_DOCK_DEFAULT_HEIGHT,
  CONSOLE_DOCK_MIN_HEIGHT,
  activateTab,
  addTab,
  clampDockHeight,
  consoleCwdForContext,
  createDockTabs,
  markTabExited,
  nextReconnectDelay,
  parsePtyFrame,
  ptyCreateOptions,
  ptyWsUrl,
  removeTab,
  renameTab,
} from './consoleDock.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 触发场景:连续新建两个 tab。期望:后建者成为激活 tab,顺序保留。
run('addTab appends and activates the new tab', () => {
  let s = createDockTabs();
  s = addTab(s, { id: 'pty-1', title: 'Terminal 1' });
  s = addTab(s, { id: 'pty-2', title: 'Terminal 2' });
  assert.equal(s.tabs.length, 2);
  assert.equal(s.activeId, 'pty-2');
  assert.deepEqual(s.tabs.map((t) => t.id), ['pty-1', 'pty-2']);
});

// 触发场景:关闭当前激活的中间 tab。期望:激活右邻;关到只剩左侧时激活左邻;
// 全关后 activeId 清空。回归:关 tab 后激活态悬空会让 xterm 容器全部隐藏。
run('removeTab activates right neighbour, then left, then empties', () => {
  let s = createDockTabs();
  s = addTab(s, { id: 'a' });
  s = addTab(s, { id: 'b' });
  s = addTab(s, { id: 'c' });
  s = activateTab(s, 'b');
  s = removeTab(s, 'b');
  assert.equal(s.activeId, 'c');
  s = removeTab(s, 'c');
  assert.equal(s.activeId, 'a');
  s = removeTab(s, 'a');
  assert.equal(s.activeId, '');
  assert.equal(s.tabs.length, 0);
});

// 触发场景:关闭非激活 tab。期望:激活 tab 不变。
run('removeTab keeps active tab when closing another', () => {
  let s = createDockTabs();
  s = addTab(s, { id: 'a' });
  s = addTab(s, { id: 'b' });
  s = removeTab(s, 'a');
  assert.equal(s.activeId, 'b');
});

// 触发场景:收到 exit 控制帧后标记 tab。期望:status/exitCode 更新,其余 tab 不动。
run('markTabExited only touches the target tab', () => {
  let s = createDockTabs();
  s = addTab(s, { id: 'a' });
  s = addTab(s, { id: 'b' });
  s = markTabExited(s, 'a', 130);
  assert.equal(s.tabs[0].status, 'exited');
  assert.equal(s.tabs[0].exitCode, 130);
  assert.equal(s.tabs[1].status, 'running');
});

// 触发场景:激活不存在的 tab id。期望:状态原样返回(防御悬空引用)。
run('activateTab ignores unknown id', () => {
  let s = createDockTabs();
  s = addTab(s, { id: 'a' });
  const before = s;
  s = activateTab(s, 'nope');
  assert.equal(s, before);
});

// 触发场景:终端内程序经 OSC 改标题(xterm onTitleChange 透传)。
// 期望:对应 tab 标题更新;空白标题忽略(保留默认 "Terminal N");
// 未知 id 原样返回。回归 bug:tab 标题不跟随 shell 标题(用户报告)。
run('renameTab follows OSC titles, ignores blank and unknown ids', () => {
  let s = createDockTabs();
  s = addTab(s, { id: 'a', title: 'Terminal 1' });
  s = renameTab(s, 'a', '  npm run dev  ');
  assert.equal(s.tabs[0].title, 'npm run dev');
  s = renameTab(s, 'a', '   ');
  assert.equal(s.tabs[0].title, 'npm run dev');
  const before = s;
  s = renameTab(s, 'ghost', 'x');
  assert.equal(s, before);
});

// 触发场景:服务端 0x00 前缀控制帧到达。期望:解析为 control + JSON payload。
// 该分流是协议核心 — 判错会把 {"cursor":N} 渲染到终端屏幕。
run('parsePtyFrame decodes 0x00-prefixed control frames', () => {
  const json = JSON.stringify({ cursor: 42 });
  const body = new TextEncoder().encode(json);
  const buf = new Uint8Array(body.length + 1);
  buf[0] = 0;
  buf.set(body, 1);
  const frame = parsePtyFrame(buf.buffer);
  assert.equal(frame.kind, 'control');
  assert.deepEqual(frame.payload, { cursor: 42 });
});

// 触发场景:普通 PTY 输出帧(首字节非 0)。期望:原样字节透传给 xterm。
run('parsePtyFrame passes through data frames', () => {
  const bytes = new TextEncoder().encode('hello[31m');
  const frame = parsePtyFrame(bytes.buffer);
  assert.equal(frame.kind, 'data');
  assert.deepEqual([...frame.bytes], [...bytes]);
});

// 触发场景:控制帧 JSON 损坏。期望:不抛异常,payload 为 null(调用方忽略)。
run('parsePtyFrame tolerates malformed control payload', () => {
  const buf = new Uint8Array([0, 0x7b, 0x6e]); // "\0{n"
  const frame = parsePtyFrame(buf.buffer);
  assert.equal(frame.kind, 'control');
  assert.equal(frame.payload, null);
});

// 触发场景:连续重连。期望:250ms 起步指数翻倍,4s 封顶(opencode 同款节奏,
// 防止 daemon 重启窗口里风暴重连)。
run('nextReconnectDelay backs off exponentially with 4s cap', () => {
  assert.equal(nextReconnectDelay(0), 250);
  assert.equal(nextReconnectDelay(1), 500);
  assert.equal(nextReconnectDelay(2), 1000);
  assert.equal(nextReconnectDelay(4), 4000);
  assert.equal(nextReconnectDelay(99), 4000);
});

// 触发场景:拖高超出视口/拖到极小/非法值。期望:夹在 [120, 视口 80%],
// 非法输入回默认高度。聊天区永远留 20% 可见。
run('clampDockHeight enforces bounds and defaults', () => {
  assert.equal(clampDockHeight(50, 1000), CONSOLE_DOCK_MIN_HEIGHT);
  assert.equal(clampDockHeight(5000, 1000), 800);
  assert.equal(clampDockHeight(300, 1000), 300);
  assert.equal(clampDockHeight(NaN, 1000), CONSOLE_DOCK_DEFAULT_HEIGHT);
});

// 触发场景:同源页面构建 WS URL。期望:ws://<host>/ws/pty/<id>?cursor=N,
// 有 token 时追加 query(浏览器 WS 没法设 header,query 是唯一通道)。
run('ptyWsUrl builds same-origin url with cursor and token', () => {
  const url = ptyWsUrl({
    id: 'pty-3', cursor: 1024, token: 'tok',
    pageProtocol: 'http:', pageHost: '127.0.0.1:28080',
  });
  assert.equal(url, 'ws://127.0.0.1:28080/ws/pty/pty-3?cursor=1024&token=tok');
});

// 触发场景:显式 origin(desktop 多 daemon 跨端口)+ 无 token + 默认 cursor。
run('ptyWsUrl honours explicit origin and defaults cursor to -1', () => {
  const url = ptyWsUrl({ id: 'pty-1', origin: 'http://127.0.0.1:36000' });
  assert.equal(url, 'ws://127.0.0.1:36000/ws/pty/pty-1?cursor=-1');
});

run('consoleCwdForContext prefers current session cwd', () => {
  const cwd = consoleCwdForContext({
    activeRef: { sessionId: 's1', cwd: ' C:/repo/session ' },
    selectedHomeWorkspace: { cwd: 'C:/repo/project' },
    health: { cwd: 'C:/repo/default' },
  });
  assert.equal(cwd, 'C:/repo/session');
});

run('consoleCwdForContext uses selected project cwd when no session is active', () => {
  const cwd = consoleCwdForContext({
    activeRef: { home: true, cwd: 'C:/repo/active' },
    selectedHomeWorkspace: { hash: 'w1', cwd: 'C:/repo/project' },
    health: { cwd: 'C:/repo/default' },
  });
  assert.equal(cwd, 'C:/repo/project');
});

run('consoleCwdForContext falls back to default cwd without session or project', () => {
  const cwd = consoleCwdForContext({
    activeRef: { home: true, cwd: 'C:/repo/active' },
    selectedHomeWorkspace: { hash: '', noWorkspace: true, cwd: '' },
    health: { cwd: 'C:/repo/default' },
  });
  assert.equal(cwd, 'C:/repo/default');
});

run('ptyCreateOptions includes cwd and shell only when present', () => {
  assert.deepEqual(ptyCreateOptions({ shellId: ' powershell ', cwd: ' C:/repo ' }), {
    shell: 'powershell',
    cwd: 'C:/repo',
  });
  assert.deepEqual(ptyCreateOptions({}), {});
});

console.log('consoleDock.test.js: all tests passed');
