import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('desktop close dialog matches the themed requested decision surface', () => {
  const dialog = source('components/DesktopCloseDialog.jsx');

  assert.match(dialog, /<Modal[\s\S]*width=\{440\}[\s\S]*layerClassName="z-\[400\]"/);
  assert.match(dialog, /className="p-4"/);
  assert.match(dialog, /className="text-\[14px\] font-semibold mb-2">关闭窗口<\/div>/);
  assert.match(dialog, /关闭窗口时您希望执行什么操作？退出应用将停止所有正在运行的任务和定时任务。/);
  assert.match(dialog, /text-\[12\.5px\] text-fg-mute leading-relaxed/);
  assert.match(dialog, /type="checkbox"[\s\S]*accent-accent/);
  assert.match(dialog, />\s*记住我的选择\s*</);
  assert.match(dialog, />\s*最小化到托盘\s*</);
  assert.match(dialog, />\s*退出应用\s*</);
  assert.match(dialog, /rounded-lg border border-border hover:bg-surface-hi transition-colors/);
  assert.match(dialog, /rounded-lg bg-accent text-white hover:opacity-90 transition-opacity/);
  assert.doesNotMatch(dialog, /rounded-full|bg-fg|text-bg|text-\[18px\]|px-6 py-6/);
  assert.doesNotMatch(dialog, /window\.confirm|window\.alert/);
});

run('desktop close dialog follows the existing close-preview confirmation style', () => {
  const dialog = source('components/DesktopCloseDialog.jsx');
  const chatView = source('components/ChatView.jsx');
  const sharedClasses = [
    'text-[14px] font-semibold mb-2',
    'text-[12.5px] text-fg-mute leading-relaxed',
    'flex justify-end gap-2',
    'px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi transition-colors',
    'px-3 py-1.5 text-[12.5px] rounded-lg bg-accent text-white hover:opacity-90 transition-opacity',
  ];

  assert.match(chatView, />关闭预览面板<\/div>/);
  for (const className of sharedClasses) {
    assert.ok(chatView.includes(className), `close-preview dialog is missing ${className}`);
    assert.ok(dialog.includes(className), `desktop close dialog is missing ${className}`);
  }
});

run('App subscribes once and preserves persist-before-action semantics', () => {
  const app = source('App.jsx');

  assert.match(app, /subscribeDesktopCloseRequest\(\(\) => \{/);
  assert.match(app, /performDesktopCloseChoice\(\{/);
  assert.match(app, /persist: setDesktopCloseBehavior/);
  assert.match(app, /hideToTray: hideDesktopToTray/);
  assert.match(app, /exitApp: requestDesktopAppExit/);
  assert.match(app, /<DesktopCloseDialog/);
});

run('General settings exposes every Desktop close behavior', () => {
  const settings = source('components/SettingsPage.jsx');

  assert.match(settings, /desktopCloseBehaviorAvailable\(\)/);
  assert.match(settings, />关闭窗口时<\/div>/);
  assert.match(settings, /DESKTOP_CLOSE_BEHAVIOR_OPTIONS\.map/);
  assert.match(settings, /setDesktopCloseBehavior\(nextBehavior\)/);
  assert.match(settings, /!closeBehaviorTrayAvailable/);
});

run('native close request and Web event use the same stable name', () => {
  const helper = source('lib/desktopCloseBehavior.js');
  const nativeMain = fs.readFileSync(
    path.resolve(srcRoot, '..', '..', 'src', 'desktop', 'main.cpp'),
    'utf8',
  );

  assert.match(helper, /acecode:desktop-close-requested/);
  assert.match(nativeMain, /acecode:desktop-close-requested/);
  assert.match(nativeMain, /resolve_close_request_action/);
  assert.match(nativeMain, /aceDesktop_hideToTray/);
  assert.match(nativeMain, /aceDesktop_quitApp/);
});

