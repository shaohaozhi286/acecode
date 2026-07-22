import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { transformSync } from '@babel/core';
import { collectStaticCopy } from '../../scripts/i18n-audit.mjs';
import localizeStaticCopyBabelPlugin from '../../scripts/localize-static-copy-babel.mjs';
import { sourceCatalogs } from './sourceCatalog.generated.js';
import { OPAQUE_STATIC_COPY } from './sourceAllowlist.js';

function sourceFiles(folder) {
  return fs.readdirSync(folder, { withFileTypes: true }).flatMap((entry) => {
    const target = path.join(folder, entry.name);
    if (entry.isDirectory()) return sourceFiles(target);
    return /\.(?:js|jsx)$/u.test(entry.name) ? [target] : [];
  });
}

const sourceRoot = path.resolve(process.cwd(), 'src');
const catalogCopy = new Set(Object.values(sourceCatalogs['zh-CN']));
const catalogKeyByCopy = new Map(
  Object.entries(sourceCatalogs['zh-CN']).map(([key, text]) => [text, key]),
);
const opaqueCopy = new Set(OPAQUE_STATIC_COPY);
const uncovered = sourceFiles(sourceRoot).flatMap((file) =>
  collectStaticCopy(fs.readFileSync(file, 'utf8'), file)
    .filter(({ text }) => !catalogCopy.has(text) && !opaqueCopy.has(text))
    .map(({ text, line }) => `${path.relative(process.cwd(), file)}:${line} ${text}`));
assert.deepEqual(uncovered, [], `uncovered static presentation copy:\n${uncovered.join('\n')}`);
for (const prompt of OPAQUE_STATIC_COPY) {
  assert.equal(catalogCopy.has(prompt), false, 'opaque prompt must bypass translation');
}
assert.equal(
  Object.values(sourceCatalogs['en-US']).some((value) => /[\u3400-\u9fff]/u.test(value)),
  false,
  'English product-copy catalog must not retain Han text',
);

const sample = `
  const dynamicUserContent = props.message;
  const prompt = ${JSON.stringify(OPAQUE_STATIC_COPY[0])};
  const comparison = value === '关闭';
  export function Fixture({ count }) {
    return <div title="关闭">新建会话<span>关闭 {count} 关闭</span>{dynamicUserContent}</div>;
  }
`;
const output = transformSync(sample, {
  filename: path.join(sourceRoot, 'components/I18nFixture.jsx'),
  configFile: false,
  babelrc: false,
  parserOpts: { plugins: ['jsx'] },
  plugins: [localizeStaticCopyBabelPlugin],
}).code;
assert.match(output, /import \{ tr as __acecodeT \} from "\/src\/i18n\/index\.js"/);
assert.match(output, /__acecodeT\("source\.[^"]+"\)/);
assert.match(output, /dynamicUserContent/);
assert.match(output, new RegExp(OPAQUE_STATIC_COPY[0].slice(0, 12)));
assert.match(output, /value === '关闭'|value === "关闭"/);
assert.match(
  output,
  /" " \+ __acecodeT\("source\.[^"]+"\)/,
  'inline JSX space before localized copy must survive compilation',
);
assert.match(
  output,
  /__acecodeT\("source\.[^"]+"\) \+ " "/,
  'inline JSX space after localized copy must survive compilation',
);

const moduleScopeFixtures = [
  {
    file: 'lib/topBarQuickActions.js',
    copy: ['新对话', '循环任务', '查找内容', '设置', '关于 ACECode', '检查更新', '退出 ACECode'],
  },
  {
    file: 'lib/sidebarNavigation.js',
    copy: ['新建任务', '循环任务', '搜索任务', '置顶任务', '任务', '工作区', 'MCP 服务器', '模型', '技能'],
  },
  {
    file: 'lib/permissionMode.js',
    copy: ['默认权限', '写/执行操作前确认', '自动接收编辑', '文件编辑自动通过,命令仍确认', '完全访问权限', '跳过所有工具权限确认'],
  },
  {
    file: 'lib/settingsNavigation.js',
    copy: ['个人', '常规', '外观', '配置', '个性化', '使用情况', '集成', '连接器', '编码', '工具', '钩子', '已归档', '已归档会话', '支持', '问题反馈', '关于'],
  },
  { file: 'components/SettingsPage.jsx', copy: ['小', '中', '大'] },
  { file: 'components/SidePanel.jsx', copy: ['变更'] },
];

for (const fixture of moduleScopeFixtures) {
  const filename = path.join(sourceRoot, fixture.file);
  const compiled = transformSync(fs.readFileSync(filename, 'utf8'), {
    filename,
    configFile: false,
    babelrc: false,
    parserOpts: { plugins: ['jsx'] },
    plugins: [localizeStaticCopyBabelPlugin],
  }).code;
  for (const text of fixture.copy) {
    const key = catalogKeyByCopy.get(text);
    assert.ok(key, `${fixture.file}: missing source catalog entry for ${text}`);
    assert.ok(
      compiled.includes(`__acecodeT("source.${key}")`),
      `${fixture.file}: module-scope product copy was not compiled: ${text}`,
    );
  }
}
console.log('[pass] static-copy compiler coverage and opaque-content boundary');
