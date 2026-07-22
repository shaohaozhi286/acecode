import fs from 'node:fs';
import path from 'node:path';
import { parseSync } from '@babel/core';

export function containsHan(value) {
  return /[\u3400-\u9fff]/u.test(value || '');
}

export function normalizeJsxText(value) {
  const lines = String(value).replace(/\r/g, '').split('\n');
  const parts = [];
  for (let index = 0; index < lines.length; index += 1) {
    let line = lines[index].replace(/\t/g, ' ');
    if (index !== 0) line = line.replace(/^ +/, '');
    if (index !== lines.length - 1) line = line.replace(/ +$/, '');
    if (!line) continue;
    parts.push(line);
  }
  return parts.join(' ').trim();
}

export function templateSource(node) {
  let value = '';
  node.quasis.forEach((quasi, index) => {
    value += quasi.value.cooked ?? quasi.value.raw;
    if (index < node.expressions.length) value += `{{p${index}}}`;
  });
  return value;
}

function ignoredFile(file) {
  const normalized = file.replaceAll('\\', '/');
  return normalized.includes('/i18n/')
    || normalized.endsWith('.test.js')
    || normalized.endsWith('.test.jsx')
    || normalized.endsWith('/runTests.js');
}

function isNonValueKey(node, parent) {
  return (parent?.type === 'ObjectProperty' || parent?.type === 'ObjectMethod'
      || parent?.type === 'ClassMethod' || parent?.type === 'ClassProperty')
    && parent.key === node && !parent.computed;
}

function ignoredLiteral(node, parent) {
  return node.type === 'StringLiteral' && (
    parent?.type === 'ImportDeclaration'
    || parent?.type === 'ExportAllDeclaration'
    || parent?.type === 'ExportNamedDeclaration'
    || parent?.type === 'Directive'
    || parent?.type === 'RegExpLiteral'
    || isNonValueKey(node, parent)
  );
}

function childNodes(node) {
  return Object.entries(node).flatMap(([key, value]) => {
    if (key === 'loc' || key === 'start' || key === 'end'
        || key === 'leadingComments' || key === 'trailingComments'
        || key === 'innerComments' || key === 'extra') return [];
    if (Array.isArray(value)) return value.filter((item) => item?.type);
    return value?.type ? [value] : [];
  });
}

export function collectStaticCopy(code, filename) {
  if (ignoredFile(filename)) return [];
  const ast = parseSync(code, {
    filename,
    sourceType: 'module',
    parserOpts: {
      plugins: ['jsx', 'classProperties', 'optionalChaining', 'topLevelAwait'],
    },
  });
  const records = [];
  function visit(node, parent = null) {
    if (node.type === 'JSXText') {
      const text = normalizeJsxText(node.value);
      if (containsHan(text)) records.push({ type: 'jsx', text, node, parent });
    } else if (node.type === 'StringLiteral' && containsHan(node.value)
        && !ignoredLiteral(node, parent)) {
      records.push({ type: 'string', text: node.value, node, parent });
    } else if (node.type === 'TemplateLiteral') {
      const text = templateSource(node);
      if (containsHan(text)) records.push({ type: 'template', text, node, parent });
    }
    for (const child of childNodes(node)) visit(child, node);
  }
  visit(ast.program);
  return records.map((record) => ({
    type: record.type,
    text: record.text,
    line: record.node.loc?.start?.line || 0,
    parentType: record.parent?.type || '',
  }));
}

function sourceFiles(root) {
  const output = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const target = path.join(root, entry.name);
    if (entry.isDirectory()) output.push(...sourceFiles(target));
    else if (/\.(?:js|jsx)$/u.test(entry.name)) output.push(target);
  }
  return output;
}

if (process.argv[1]?.replaceAll('\\', '/').endsWith('/i18n-audit.mjs')
    || process.argv[1] === 'scripts/i18n-audit.mjs') {
  const srcRoot = path.resolve(process.cwd(), 'src');
  const rows = sourceFiles(srcRoot).flatMap((file) =>
    collectStaticCopy(fs.readFileSync(file, 'utf8'), file).map((record) => ({
      file: path.relative(process.cwd(), file).replaceAll('\\', '/'),
      ...record,
    })));
  if (process.argv.includes('--json')) {
    process.stdout.write(`${JSON.stringify(rows, null, 2)}\n`);
  } else {
    for (const row of rows) {
      process.stdout.write(`${row.file}:${row.line}\t${row.type}\t${row.parentType}\t${JSON.stringify(row.text)}\n`);
    }
    process.stderr.write(`static Han copy: ${rows.length} occurrences, ${new Set(rows.map((row) => row.text)).size} unique\n`);
  }
}
