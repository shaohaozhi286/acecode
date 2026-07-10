#!/usr/bin/env node
'use strict';
// @acecode/desktop 启动垫片:定位当前平台的 @acecode/<os>-<cpu> 平台包,
// 以「分离进程」方式启动桌面壳(Windows/Linux: acecode-desktop;macOS: ACECode.app),
// 随后本进程立即退出。
//
// 桌面壳会按「自身所在目录」寻找 acecode(daemon 主程序),平台包把两者放在同一
// 目录里,所以必须直接启动平台包内的原始文件,不能拷贝或链接到别处。

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const SCOPE = '@acecode';
const RELEASES_URL = 'https://github.com/shaohaozhi286/acecode/releases';
const SUPPORTED = new Set([
  'linux-x64',
  'linux-arm64',
  'darwin-x64',
  'darwin-arm64',
  'win32-x64',
]);

function fail(message) {
  console.error(`acecode-desktop: ${message}`);
  process.exit(1);
}

function resolvePlatformDir() {
  const key = `${process.platform}-${process.arch}`;
  if (!SUPPORTED.has(key)) {
    fail(
      `没有 ${key} 平台的预编译二进制。请从 GitHub Releases 下载对应压缩包:\n  ${RELEASES_URL}`
    );
  }
  const pkg = `${SCOPE}/${key}`;
  let manifestPath;
  try {
    manifestPath = require.resolve(`${pkg}/package.json`);
  } catch {
    fail(
      `平台包 ${pkg} 未安装。\n` +
        `若安装时使用了 --omit=optional / --no-optional,请去掉该参数重新安装;\n` +
        `否则请尝试重新安装 ${SCOPE}/desktop。`
    );
  }
  return path.dirname(manifestPath);
}

const platformDir = resolvePlatformDir();
const args = process.argv.slice(2);

let child;
if (process.platform === 'darwin') {
  const app = path.join(platformDir, 'ACECode.app');
  if (!fs.existsSync(app)) {
    fail(`平台包不完整:找不到 ${app},请重新安装 ${SCOPE}/desktop。`);
  }
  const openArgs = args.length ? [app, '--args', ...args] : [app];
  child = spawn('open', openArgs, { stdio: 'ignore', detached: true });
} else {
  const exe = path.join(
    platformDir,
    process.platform === 'win32' ? 'acecode-desktop.exe' : 'acecode-desktop'
  );
  if (!fs.existsSync(exe)) {
    fail(`平台包不完整:找不到 ${exe},请重新安装 ${SCOPE}/desktop。`);
  }
  child = spawn(exe, args, { stdio: 'ignore', detached: true });
}

child.on('error', (err) => fail(`无法启动桌面壳: ${err.message}`));
child.unref();
console.log('ACECode Desktop 正在启动…');
