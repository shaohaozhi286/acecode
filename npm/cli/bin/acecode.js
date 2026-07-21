#!/usr/bin/env node
'use strict';
// acecode 启动垫片:按当前平台解析 @aceagent/<os>-<cpu> 平台包里的真实二进制并透传运行。
//
// 真实二进制会按「自身所在目录」定位 ace-browser-host / acecode-desktop 等同伴文件,
// 因此必须直接 spawn 平台包内的原始文件,不能把它拷贝或链接到别处再执行。

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const SCOPE = '@aceagent';
const RELEASES_URL = 'https://github.com/shaohaozhi286/acecode/releases';
const SUPPORTED = new Set([
  'linux-x64',
  'linux-arm64',
  'darwin-x64',
  'darwin-arm64',
  'win32-x64',
  'win32-arm64',
]);

function fail(message) {
  console.error(`acecode: ${message}`);
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
        '否则请尝试重新安装 acecode。'
    );
  }
  return path.dirname(manifestPath);
}

const platformDir = resolvePlatformDir();
const exe = path.join(
  platformDir,
  process.platform === 'win32' ? 'acecode.exe' : 'acecode'
);
if (!fs.existsSync(exe)) {
  fail(`平台包不完整:找不到 ${exe},请重新安装 acecode。`);
}

const result = spawnSync(exe, process.argv.slice(2), { stdio: 'inherit' });
if (result.error) {
  fail(`无法启动 ${exe}: ${result.error.message}`);
}
process.exit(result.status === null ? 1 : result.status);
