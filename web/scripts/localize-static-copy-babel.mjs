import { sourceCatalogs } from '../src/i18n/sourceCatalog.generated.js';
import { normalizeJsxText, templateSource } from './i18n-audit.mjs';

const sourceKeyByText = new Map(
  Object.entries(sourceCatalogs['zh-CN']).map(([key, text]) => [text, key]),
);

function ignoredFilename(filename = '') {
  const normalized = filename.replaceAll('\\', '/');
  return normalized.includes('/src/i18n/')
    || normalized.endsWith('.test.js')
    || normalized.endsWith('.test.jsx')
    || normalized.endsWith('/runTests.js');
}

function isComparison(path) {
  if (!path.parentPath?.isBinaryExpression()) return false;
  return ['==', '===', '!=', '!==', 'in', 'instanceof']
    .includes(path.parentPath.node.operator);
}

function isNonValueKey(path) {
  const parent = path.parentPath;
  return (parent?.isObjectProperty() || parent?.isObjectMethod()
      || parent?.isClassMethod() || parent?.isClassProperty())
    && parent.node.key === path.node && !parent.node.computed;
}

function shouldSkipString(path) {
  const parent = path.parentPath;
  return parent?.isImportDeclaration()
    || parent?.isExportAllDeclaration()
    || parent?.isExportNamedDeclaration()
    || parent?.isDirective()
    || parent?.isSwitchCase({ test: path.node })
    || isNonValueKey(path)
    || isComparison(path);
}

function translationCall(types, text, expressions = []) {
  const key = sourceKeyByText.get(text);
  if (!key) return null;
  const args = [types.stringLiteral(`source.${key}`)];
  if (expressions.length) {
    args.push(types.objectExpression(expressions.map((expression, index) =>
      types.objectProperty(types.identifier(`p${index}`), expression))));
  }
  return types.callExpression(types.identifier('__acecodeT'), args);
}

function preserveInlineJsxSpacing(types, expression, rawText) {
  // Babel/React preserves a same-line space beside an expression, while
  // indentation-only whitespace around line breaks is discarded. JSXText is
  // replaced by one expression container here, so carry those meaningful
  // inline edge spaces into the resulting string explicitly.
  const leadingSpace = /^[\t ]+[^\r\n]/u.test(rawText);
  const trailingSpace = /[^\r\n][\t ]+$/u.test(rawText);
  let result = expression;
  if (leadingSpace) {
    result = types.binaryExpression('+', types.stringLiteral(' '), result);
  }
  if (trailingSpace) {
    result = types.binaryExpression('+', result, types.stringLiteral(' '));
  }
  return result;
}

export default function localizeStaticCopyBabelPlugin({ types }) {
  return {
    name: 'acecode-localize-static-copy',
    pre(file) {
      this.acecodeSkip = ignoredFilename(file.opts.filename || '');
      this.acecodeLocalized = false;
    },
    visitor: {
      JSXText(path, state) {
        if (state.acecodeSkip) return;
        const text = normalizeJsxText(path.node.value);
        const call = translationCall(types, text);
        if (!call) return;
        state.acecodeLocalized = true;
        path.replaceWith(types.jsxExpressionContainer(
          preserveInlineJsxSpacing(types, call, path.node.value),
        ));
        path.skip();
      },
      StringLiteral(path, state) {
        if (state.acecodeSkip || shouldSkipString(path)) return;
        const call = translationCall(types, path.node.value);
        if (!call) return;
        state.acecodeLocalized = true;
        if (path.parentPath?.isJSXAttribute()) {
          path.replaceWith(types.jsxExpressionContainer(call));
        } else {
          path.replaceWith(call);
        }
        path.skip();
      },
      TemplateLiteral(path, state) {
        if (state.acecodeSkip) return;
        const call = translationCall(types, templateSource(path.node), path.node.expressions);
        if (!call) return;
        state.acecodeLocalized = true;
        path.replaceWith(call);
        path.skip();
      },
      Program: {
        exit(path, state) {
          if (!state.acecodeLocalized) return;
          path.unshiftContainer('body', types.importDeclaration([
            types.importSpecifier(
              types.identifier('__acecodeT'),
              types.identifier('tr'),
            ),
          ], types.stringLiteral('/src/i18n/index.js')));
        },
      },
    },
  };
}
