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

function moduleScopeObjectGetter(types, path, call) {
  if (path.getFunctionParent()) return null;
  const property = path.parentPath;
  if (!property?.isObjectProperty() || property.node.value !== path.node) return null;
  const getter = types.objectMethod(
    'get',
    types.cloneNode(property.node.key, true),
    [],
    types.blockStatement([types.returnStatement(call)]),
    property.node.computed,
  );
  types.inheritsComments(getter, property.node);
  return { property, getter };
}

function moduleScopeLocalizedArray(types, path) {
  if (path.getFunctionParent()
      || path.node.elements.some((element) => types.isSpreadElement(element))) {
    return null;
  }
  const readers = [];
  const values = path.node.elements.map((element, index) => {
    if (!types.isStringLiteral(element)) return element;
    const call = translationCall(types, element.value);
    if (!call) return element;
    readers.push(types.objectProperty(
      types.numericLiteral(index),
      types.arrowFunctionExpression([], call),
    ));
    return types.unaryExpression('void', types.numericLiteral(0));
  });
  if (readers.length === 0) return null;
  return types.callExpression(
    types.identifier('__acecodeLocalizedArray'),
    [types.arrayExpression(values), types.objectExpression(readers)],
  );
}

function assertNotEagerModuleTranslation(path) {
  if (path.getFunctionParent()) return;
  throw path.buildCodeFrameError(
    'Module-scope translated primitives must resolve lazily; '
    + 'move this copy behind a function or a supported object/array accessor.',
  );
}

function isUseMemoCall(path) {
  const callee = path.node.callee;
  return path.isCallExpression()
    && ((callee.type === 'Identifier' && callee.name === 'useMemo')
      || (callee.type === 'MemberExpression'
        && !callee.computed
        && callee.object.type === 'Identifier'
        && callee.object.name === 'React'
        && callee.property.type === 'Identifier'
        && callee.property.name === 'useMemo'));
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
      this.acecodeLocalizedArray = false;
      this.acecodeMemoDependencyArrays = [];
    },
    visitor: {
      CallExpression(path, state) {
        if (state.acecodeSkip || !isUseMemoCall(path)) return;
        const dependencies = path.get('arguments.1');
        if (!dependencies?.isArrayExpression()) return;
        state.acecodeMemoDependencyArrays.push(dependencies);
      },
      ArrayExpression(path, state) {
        if (state.acecodeSkip) return;
        const call = moduleScopeLocalizedArray(types, path);
        if (!call) return;
        state.acecodeLocalized = true;
        state.acecodeLocalizedArray = true;
        path.replaceWith(call);
      },
      JSXText(path, state) {
        if (state.acecodeSkip) return;
        const text = normalizeJsxText(path.node.value);
        const call = translationCall(types, text);
        if (!call) return;
        assertNotEagerModuleTranslation(path);
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
        const lazyProperty = moduleScopeObjectGetter(types, path, call);
        if (lazyProperty) {
          lazyProperty.property.replaceWith(lazyProperty.getter);
          lazyProperty.property.skip();
          return;
        }
        assertNotEagerModuleTranslation(path);
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
        assertNotEagerModuleTranslation(path);
        state.acecodeLocalized = true;
        path.replaceWith(call);
        path.skip();
      },
      Program: {
        exit(path, state) {
          if (!state.acecodeLocalized) return;
          const memoDependencyArrays = state.acecodeMemoDependencyArrays
            .filter((dependencies) => dependencies?.node);
          memoDependencyArrays.forEach((dependencies) => {
            dependencies.pushContainer('elements', types.memberExpression(
              types.identifier('__acecodeI18n'),
              types.identifier('resolvedLanguage'),
            ));
          });
          const specifiers = [types.importSpecifier(
            types.identifier('__acecodeT'),
            types.identifier('tr'),
          )];
          if (state.acecodeLocalizedArray) {
            specifiers.push(types.importSpecifier(
              types.identifier('__acecodeLocalizedArray'),
              types.identifier('localizedArray'),
            ));
          }
          if (memoDependencyArrays.length > 0) {
            specifiers.push(types.importSpecifier(
              types.identifier('__acecodeI18n'),
              types.identifier('i18n'),
            ));
          }
          path.unshiftContainer('body', types.importDeclaration(
            specifiers,
            types.stringLiteral('/src/i18n/index.js'),
          ));
        },
      },
    },
  };
}
