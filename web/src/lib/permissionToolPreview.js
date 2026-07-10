// 权限确认弹窗的工具展示口径(纯函数,Node 单测)。
// 与 TUI 侧保持一致:工具名走 pascal_case_tool_name 同款转换
// (src/tui/tool_row_format.cpp),file_write/file_edit 只提示
// 「改哪个文件、多少行」,不再把参数 JSON(含完整文件内容)糊进弹窗。

// 对齐 C++ pascal_case_tool_name:下划线作分词符丢弃,词首小写字母转大写,
// 已是驼峰的名字(AskUserQuestion / EnterWorktree)原样保留。
export function pascalCaseToolName(name) {
  const s = String(name ?? '');
  let out = '';
  let upperNext = true;
  for (const ch of s) {
    if (ch === '_') {
      upperNext = true;
      continue;
    }
    out += upperNext && ch >= 'a' && ch <= 'z' ? ch.toUpperCase() : ch;
    upperNext = false;
  }
  return out;
}

// 行数统计:空串 0 行;结尾单个换行不额外计一行("a\n" 是 1 行)。
export function countLines(text) {
  const s = String(text ?? '');
  if (!s) return 0;
  const body = s.endsWith('\n') ? s.slice(0, -1) : s;
  return body === '' ? 1 : body.split('\n').length;
}

// 返回 { toolLabel, kind, ... }:
//   kind='file'    → filePath + detail(文件写入/编辑的精简摘要)
//   kind='command' → command(bash,命令本身就是要审的内容)
//   kind='json'    → 无更好渲染的工具,调用方回退到紧凑 JSON 预览
export function buildPermissionToolPreview(tool, args) {
  const toolLabel = pascalCaseToolName(tool || 'tool') || 'Tool';
  const a = args && typeof args === 'object' && !Array.isArray(args) ? args : null;

  if (tool === 'file_write' && a && typeof a.file_path === 'string' && a.file_path) {
    return {
      toolLabel,
      kind: 'file',
      filePath: a.file_path,
      detail: `写入 ${countLines(a.content)} 行`,
    };
  }

  if (tool === 'file_edit' && a && typeof a.file_path === 'string' && a.file_path) {
    const oldLines = countLines(a.old_string);
    const newLines = countLines(a.new_string);
    let detail;
    if (oldLines === 0) {
      detail = `写入 ${newLines} 行`;
    } else if (newLines === 0) {
      detail = `删除 ${oldLines} 行`;
    } else {
      detail = `替换 ${oldLines} 行 → ${newLines} 行`;
    }
    if (a.replace_all === true) detail += '(所有匹配)';
    return { toolLabel, kind: 'file', filePath: a.file_path, detail };
  }

  if (tool === 'bash' && a && typeof a.command === 'string') {
    return { toolLabel, kind: 'command', command: a.command };
  }

  return { toolLabel, kind: 'json' };
}
