#!/usr/bin/env node
// 把 CI 打包产物(解包后的 acecode-<platform>/ 目录)组装成可直接 npm publish 的目录树。
//
// 用法(CI 的 publish-npm job 调用,也可本地对着解包目录跑):
//   node scripts/npm/prepare-npm-packages.mjs --version 0.6.8 --input extracted --output npm-staging
//
// 输入布局(与 package.yml 的 Package 步骤产物一致):
//   extracted/acecode-linux-x64/{acecode,ace-browser-host,acecode-desktop,acecode-logo.png,...}
//   extracted/acecode-windows-x64/{acecode.exe,ace-browser-host.exe,acecode-desktop.exe,...}
//   extracted/acecode-macos-arm64/{acecode,ace-browser-host,ACECode.app/,...}
//
// 输出布局(发布顺序:先 platform/* 再 cli / desktop):
//   npm-staging/platform/<os>-<cpu>/   六个平台二进制包 @aceagent/<os>-<cpu>
//   npm-staging/cli/                   acecode(模板拷贝 + 版本/optionalDependencies 盖章)
//   npm-staging/desktop/               @aceagent/desktop(同上)
//
// 设计约束:
// - 平台包同时装下 acecode / ace-browser-host / acecode-desktop(或 ACECode.app):
//   三个二进制都按「自身所在目录」互相定位,拆开会破坏运行时解析。
// - 改包名/scope 时:改这里的 SCOPE 常量 + npm/cli/package.json 与
//   npm/desktop/package.json 的 name 字段 + 两个 bin/*.js 里的 SCOPE 常量。

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const SCOPE = '@aceagent';
const CLI_PACKAGE = '@aceagent/acecode';
const DESKTOP_PACKAGE = `${SCOPE}/desktop`;
const REPO_URL = 'https://github.com/shaohaozhi286/acecode';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

// ciId 对应 package.yml 的 matrix.id;files 是必须存在并被拷入平台包的文件清单。
const PLATFORMS = [
  {
    ciId: 'linux-x64',
    os: 'linux',
    cpu: 'x64',
    files: ['acecode', 'ace-browser-host', 'acecode-desktop', 'acecode-logo.png'],
    executables: ['acecode', 'ace-browser-host', 'acecode-desktop'],
  },
  {
    ciId: 'linux-arm64',
    os: 'linux',
    cpu: 'arm64',
    files: ['acecode', 'ace-browser-host', 'acecode-desktop', 'acecode-logo.png'],
    executables: ['acecode', 'ace-browser-host', 'acecode-desktop'],
  },
  {
    ciId: 'windows-x64',
    os: 'win32',
    cpu: 'x64',
    files: ['acecode.exe', 'ace-browser-host.exe', 'acecode-desktop.exe'],
    executables: [],
  },
  {
    ciId: 'windows-arm64',
    os: 'win32',
    cpu: 'arm64',
    files: ['acecode.exe', 'ace-browser-host.exe', 'acecode-desktop.exe'],
    executables: [],
  },
  {
    ciId: 'macos-x64',
    os: 'darwin',
    cpu: 'x64',
    files: ['acecode', 'ace-browser-host', 'ACECode.app'],
    executables: ['acecode', 'ace-browser-host'],
  },
  {
    ciId: 'macos-arm64',
    os: 'darwin',
    cpu: 'arm64',
    files: ['acecode', 'ace-browser-host', 'ACECode.app'],
    executables: ['acecode', 'ace-browser-host'],
  },
];

function parseArgs(argv) {
  const args = { version: '', input: '', output: '' };
  for (let i = 2; i < argv.length; i++) {
    const key = argv[i];
    const value = argv[i + 1];
    if (key === '--version') args.version = value ?? '';
    else if (key === '--input') args.input = value ?? '';
    else if (key === '--output') args.output = value ?? '';
    else continue;
    i++;
  }
  if (!args.version || !args.input || !args.output) {
    console.error(
      '用法: node scripts/npm/prepare-npm-packages.mjs --version <semver> --input <extracted-dir> --output <staging-dir>'
    );
    process.exit(2);
  }
  // 与 git tag v<semver> 对齐;宽松校验,预发布号(0.7.0-rc1)合法。
  if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(args.version)) {
    console.error(`非法版本号: ${args.version}`);
    process.exit(2);
  }
  return args;
}

function writeJson(filePath, data) {
  fs.writeFileSync(filePath, JSON.stringify(data, null, 2) + '\n');
}

function chmodExecutableRecursive(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) chmodExecutableRecursive(p);
    else fs.chmodSync(p, 0o755);
  }
}

function buildPlatformPackage(platform, version, inputRoot, outputRoot) {
  const srcDir = path.join(inputRoot, `acecode-${platform.ciId}`);
  if (!fs.existsSync(srcDir)) {
    throw new Error(`缺少输入目录: ${srcDir}`);
  }
  const pkgName = `${SCOPE}/${platform.os}-${platform.cpu}`;
  const outDir = path.join(outputRoot, 'platform', `${platform.os}-${platform.cpu}`);
  fs.mkdirSync(outDir, { recursive: true });

  for (const file of platform.files) {
    const src = path.join(srcDir, file);
    const dst = path.join(outDir, file);
    if (!fs.existsSync(src)) {
      throw new Error(`平台 ${platform.ciId} 缺少产物文件: ${src}`);
    }
    fs.cpSync(src, dst, { recursive: true });
  }
  for (const exe of platform.executables) {
    fs.chmodSync(path.join(outDir, exe), 0o755);
  }
  if (platform.os === 'darwin') {
    const macosDir = path.join(outDir, 'ACECode.app', 'Contents', 'MacOS');
    if (!fs.existsSync(path.join(macosDir, 'ACECode'))) {
      throw new Error(`平台 ${platform.ciId} 的 ACECode.app 不完整: 缺少 ${macosDir}/ACECode`);
    }
    chmodExecutableRecursive(macosDir);
  }

  writeJson(path.join(outDir, 'package.json'), {
    name: pkgName,
    version,
    description: `ACECode prebuilt binaries for ${platform.os}-${platform.cpu} (installed automatically by ${CLI_PACKAGE} and ${DESKTOP_PACKAGE})`,
    homepage: REPO_URL,
    repository: { type: 'git', url: `git+${REPO_URL}.git` },
    license: 'SEE LICENSE IN README.md',
    os: [platform.os],
    cpu: [platform.cpu],
    preferUnplugged: true,
  });
  fs.writeFileSync(
    path.join(outDir, 'README.md'),
    `# ${pkgName}\n\nACECode ${platform.os}-${platform.cpu} 预编译二进制。` +
      `请不要直接安装本包,安装 [${CLI_PACKAGE}](https://www.npmjs.com/package/${CLI_PACKAGE}) 或 ` +
      `[${DESKTOP_PACKAGE}](https://www.npmjs.com/package/${DESKTOP_PACKAGE}) 即可按平台自动获取。\n\n` +
      `源码与许可: <${REPO_URL}>\n`
  );
  return pkgName;
}

function buildMainPackage(templateDirName, version, optionalDependencies, outputRoot) {
  const templateDir = path.join(REPO_ROOT, 'npm', templateDirName);
  const outDir = path.join(outputRoot, templateDirName);
  fs.cpSync(templateDir, outDir, { recursive: true });

  const manifestPath = path.join(outDir, 'package.json');
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  manifest.version = version;
  manifest.optionalDependencies = optionalDependencies;
  writeJson(manifestPath, manifest);
  return manifest.name;
}

function main() {
  const { version, input, output } = parseArgs(process.argv);
  const inputRoot = path.resolve(input);
  const outputRoot = path.resolve(output);
  fs.rmSync(outputRoot, { recursive: true, force: true });
  fs.mkdirSync(outputRoot, { recursive: true });

  const optionalDependencies = {};
  for (const platform of PLATFORMS) {
    const name = buildPlatformPackage(platform, version, inputRoot, outputRoot);
    optionalDependencies[name] = version;
    console.log(`platform package ready: ${name}@${version}`);
  }

  for (const templateDirName of ['cli', 'desktop']) {
    const name = buildMainPackage(templateDirName, version, optionalDependencies, outputRoot);
    console.log(`main package ready: ${name}@${version}`);
  }

  console.log(`\n发布目录已就绪: ${outputRoot}`);
  console.log('发布顺序: platform/* → cli → desktop');
}

main();
