import assert from 'node:assert/strict';
import {
  pascalCaseToolName,
  countLines,
  buildPermissionToolPreview,
} from './permissionToolPreview.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:权限弹窗头部显示工具名。
// 期望:与 TUI 的 pascal_case_tool_name 同口径 —— 下划线分词转 PascalCase,
// 已是驼峰的内置名原样保留,MCP 双下划线名同样逐词转换。
run('pascalCaseToolName 与 TUI 口径一致', () => {
  assert.equal(pascalCaseToolName('file_write'), 'FileWrite');
  assert.equal(pascalCaseToolName('bash'), 'Bash');
  assert.equal(pascalCaseToolName('AskUserQuestion'), 'AskUserQuestion');
  assert.equal(pascalCaseToolName('EnterWorktree'), 'EnterWorktree');
  assert.equal(pascalCaseToolName('mcp__server__snapshot'), 'McpServerSnapshot');
  assert.equal(pascalCaseToolName(''), '');
});

// 场景:统计 file_write content / file_edit old|new_string 的行数。
// 期望:空串 0 行;结尾单个换行不额外计行("a\n" 是 1 行不是 2 行)——
// 否则写入整文件(必带尾换行)时行数恒虚高 1。
run('countLines 行数统计边界', () => {
  assert.equal(countLines(''), 0);
  assert.equal(countLines(null), 0);
  assert.equal(countLines('a'), 1);
  assert.equal(countLines('a\n'), 1);
  assert.equal(countLines('a\nb'), 2);
  assert.equal(countLines('a\nb\n'), 2);
  assert.equal(countLines('\n'), 1);
});

// 场景:file_write 请求确认(弹窗此前直接糊整个参数 JSON,含完整文件内容)。
// 期望:kind='file',只给文件路径 + 「写入 N 行」,不透出 content 本体。
run('file_write 摘要为 路径 + 写入行数', () => {
  const view = buildPermissionToolPreview('file_write', {
    file_path: 'C:/proj/main.go',
    content: 'package main\nfunc main() {}\n',
  });
  assert.equal(view.toolLabel, 'FileWrite');
  assert.equal(view.kind, 'file');
  assert.equal(view.filePath, 'C:/proj/main.go');
  assert.equal(view.detail, '写入 2 行');
});

// 场景:file_edit 三种形态 —— 替换 / 新建(old 空)/ 删除(new 空),
// 以及 replace_all 全量替换。
// 期望:detail 分别为「替换 N 行 → M 行」「写入 M 行」「删除 N 行」,
// replace_all 追加「(所有匹配)」提示。
run('file_edit 摘要覆盖 替换/新建/删除/全量', () => {
  const replace = buildPermissionToolPreview('file_edit', {
    file_path: 'a.ts',
    old_string: 'x\ny\nz',
    new_string: 'x\nz',
  });
  assert.equal(replace.toolLabel, 'FileEdit');
  assert.equal(replace.kind, 'file');
  assert.equal(replace.detail, '替换 3 行 → 2 行');

  const create = buildPermissionToolPreview('file_edit', {
    file_path: 'a.ts',
    old_string: '',
    new_string: 'x\ny',
  });
  assert.equal(create.detail, '写入 2 行');

  const remove = buildPermissionToolPreview('file_edit', {
    file_path: 'a.ts',
    old_string: 'x\ny',
    new_string: '',
  });
  assert.equal(remove.detail, '删除 2 行');

  const all = buildPermissionToolPreview('file_edit', {
    file_path: 'a.ts',
    old_string: 'x',
    new_string: 'y',
    replace_all: true,
  });
  assert.equal(all.detail, '替换 1 行 → 1 行(所有匹配)');
});

// 场景:bash 请求确认 —— 命令本身就是用户要审的内容。
// 期望:kind='command',原样透出 command(截断由渲染层负责)。
run('bash 透出命令本体', () => {
  const view = buildPermissionToolPreview('bash', { command: 'rm -rf build' });
  assert.equal(view.toolLabel, 'Bash');
  assert.equal(view.kind, 'command');
  assert.equal(view.command, 'rm -rf build');
});

// 场景:其他工具(MCP / memory_write 等)以及参数缺失/异形的降级路径 ——
// file_path 缺失、args 是字符串(后端 JSON parse 失败原样透传)、args 是数组。
// 期望:一律 kind='json' 回退到紧凑 JSON 预览,不抛异常。
run('未识别工具与异形参数回退 json', () => {
  assert.equal(buildPermissionToolPreview('memory_write', { path: 'x' }).kind, 'json');
  assert.equal(buildPermissionToolPreview('file_write', {}).kind, 'json');
  assert.equal(buildPermissionToolPreview('file_edit', { old_string: 'x' }).kind, 'json');
  assert.equal(buildPermissionToolPreview('bash', 'not-json').kind, 'json');
  assert.equal(buildPermissionToolPreview('file_write', ['a']).kind, 'json');
  const fallback = buildPermissionToolPreview(undefined, undefined);
  assert.equal(fallback.kind, 'json');
  assert.equal(fallback.toolLabel, 'Tool');
});
