import assert from 'node:assert/strict';
import { flattenCommands } from './slashCommands.js';
import {
  COMPOSER_COMMAND_TAG,
  COMPOSER_PATH_TAG,
  clipboardHasRichText,
  composerAdjacentTagDeletionRange,
  composerDocumentFromText,
  composerDocumentWithSynchronizedLeadingCommand,
  composerPlainTextOffsetForPoint,
  composerPlainTextRangeFromSelection,
  composerPointForPlainTextOffset,
  composerSelectionFromPlainTextRange,
  composerTextFromDocument,
  normalizeComposerPlainText,
  plainTextFromClipboardData,
  richComposerModelFromText,
  richComposerTextFromModel,
} from './richComposerModel.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const COMMANDS = flattenCommands({
  builtins: [{ name: 'init', description: 'Generate AGENT.md' }],
  commands: [{ name: 'opsx-apply', description: 'Apply OpenSpec change' }],
  skills: [{ name: 'openspec-explore', description: 'Explore a change' }],
});

run('normalizeComposerPlainText normalizes CRLF and CR to LF', () => {
  assert.equal(normalizeComposerPlainText('a\r\nb\rc'), 'a\nb\nc');
});

run('rich composer model round-trips empty and multiline plain text', () => {
  assert.equal(richComposerTextFromModel(richComposerModelFromText('', COMMANDS)), '');
  const text = 'first\nsecond\nthird';
  assert.deepEqual(richComposerModelFromText(text, COMMANDS), {
    kind: 'plain',
    text,
    command: null,
    rest: text,
  });
  assert.equal(richComposerTextFromModel(richComposerModelFromText(text, COMMANDS)), text);
});

run('recognized leading command becomes command model and serializes with slash', () => {
  const model = richComposerModelFromText('/openspec-explore investigate input', COMMANDS);
  assert.equal(model.kind, 'command');
  assert.equal(model.command.token, '/openspec-explore');
  assert.equal(model.command.name, 'openspec-explore');
  assert.equal(model.rest, ' investigate input');
  assert.equal(richComposerTextFromModel(model), '/openspec-explore investigate input');
});

run('recognized opencode command keeps command kind', () => {
  const model = richComposerModelFromText('/opsx-apply change-123', COMMANDS);
  assert.equal(model.kind, 'command');
  assert.equal(model.command.name, 'opsx-apply');
  assert.equal(model.command.kind, 'command');
  assert.equal(model.rest, ' change-123');
});

run('unknown leading command remains plain text', () => {
  const text = '/not-known investigate input';
  const model = richComposerModelFromText(text, COMMANDS);
  assert.equal(model.kind, 'plain');
  assert.equal(model.command, null);
  assert.equal(richComposerTextFromModel(model), text);
});

run('Slate composer document round-trips command, path tags, and multiline text', () => {
  const text = '/openspec-explore inspect @src/main.cpp now\nthen @"docs/file name.md" and @"C:/Outside Folder/"';
  const document = composerDocumentFromText(text, COMMANDS);
  assert.equal(composerTextFromDocument(document), text);
  assert.equal(document.length, 2);
  assert.equal(document[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), true);
  assert.equal(document[0].children.some((child) => child.type === COMPOSER_PATH_TAG), true);
  const quoted = document[1].children.find((child) => child.type === COMPOSER_PATH_TAG);
  assert.equal(quoted.token, '@"docs/file name.md"');
  assert.equal(quoted.path, 'docs/file name.md');
  const absolute = document[1].children.filter((child) => child.type === COMPOSER_PATH_TAG)[1];
  assert.equal(absolute.token, '@"C:/Outside Folder/"');
  assert.equal(absolute.path, 'C:/Outside Folder/');
  assert.equal(absolute.directory, true);
});

run('Slate composer keeps unfinished directory queries and non-reference at-signs editable', () => {
  const unfinished = composerDocumentFromText('continue @src/', COMMANDS);
  const email = composerDocumentFromText('mail a@b.com', COMMANDS);
  assert.equal(composerTextFromDocument(unfinished), 'continue @src/');
  assert.equal(composerTextFromDocument(email), 'mail a@b.com');
  assert.equal(unfinished.flatMap((block) => block.children).some((child) => child.type === COMPOSER_PATH_TAG), false);
  assert.equal(email.flatMap((block) => block.children).some((child) => child.type === COMPOSER_PATH_TAG), false);
});

run('leading command synchronization preserves an unfinished path query as text', () => {
  const text = '/openspec-explore continue @src/';
  const liveDocument = [{
    type: 'paragraph',
    children: [{ text }],
  }];
  const synchronized = composerDocumentWithSynchronizedLeadingCommand(liveDocument, text, COMMANDS);
  assert.equal(composerTextFromDocument(synchronized), text);
  assert.equal(synchronized[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), true);
  assert.equal(synchronized[0].children.some((child) => child.type === COMPOSER_PATH_TAG), false);
});

run('plain-text offsets clamp around inline void tags and preserve newlines', () => {
  const text = '/openspec-explore @src/main.cpp\nnext';
  const document = composerDocumentFromText(text, COMMANDS);
  const commandEnd = '/openspec-explore'.length;
  const pathStart = commandEnd + 1;
  const pathEnd = pathStart + '@src/main.cpp'.length;
  const secondLineStart = pathEnd + 1;

  const beforeCommand = composerPointForPlainTextOffset(document, 0, 'backward');
  assert.equal(composerPlainTextOffsetForPoint(document, beforeCommand), 0);
  const insideCommandBackward = composerPointForPlainTextOffset(document, 4, 'backward');
  const insideCommandForward = composerPointForPlainTextOffset(document, 4, 'forward');
  assert.equal(composerPlainTextOffsetForPoint(document, insideCommandBackward), 0);
  assert.equal(composerPlainTextOffsetForPoint(document, insideCommandForward), commandEnd);

  const insidePathBackward = composerPointForPlainTextOffset(document, pathStart + 2, 'backward');
  const insidePathForward = composerPointForPlainTextOffset(document, pathStart + 2, 'forward');
  assert.equal(composerPlainTextOffsetForPoint(document, insidePathBackward), pathStart);
  assert.equal(composerPlainTextOffsetForPoint(document, insidePathForward), pathEnd);
  assert.equal(
    composerPlainTextOffsetForPoint(document, composerPointForPlainTextOffset(document, secondLineStart)),
    secondLineStart,
  );
});

run('plain-text range mapping preserves collapsed and backward selections around tags', () => {
  const text = '/openspec-explore @src/main.cpp';
  const document = composerDocumentFromText(text, COMMANDS);
  const pathStart = '/openspec-explore '.length;
  const pathEnd = text.length;

  const collapsed = composerSelectionFromPlainTextRange(document, pathStart + 2, pathStart + 2);
  assert.deepEqual(collapsed.anchor, collapsed.focus);
  assert.equal(composerPlainTextOffsetForPoint(document, collapsed.anchor), pathEnd);

  const backward = composerSelectionFromPlainTextRange(document, pathStart, pathEnd, 'backward');
  assert.deepEqual(composerPlainTextRangeFromSelection(document, backward), {
    start: pathStart,
    end: pathEnd,
    direction: 'backward',
  });
});

run('atomic deletion range includes a tag and its following separator', () => {
  const text = '/openspec-explore continue @src/main.cpp now';
  const document = composerDocumentFromText(text, COMMANDS);
  const commandEnd = '/openspec-explore'.length;
  const afterCommandSpace = composerSelectionFromPlainTextRange(
    document,
    commandEnd + 1,
    commandEnd + 1,
  );
  assert.deepEqual(composerAdjacentTagDeletionRange(
    document,
    afterCommandSpace,
    'backward',
  ), {
    start: 0,
    end: commandEnd + 1,
  });

  const pathStart = text.indexOf('@src/main.cpp');
  const beforePath = composerSelectionFromPlainTextRange(document, pathStart, pathStart);
  assert.deepEqual(composerAdjacentTagDeletionRange(document, beforePath, 'forward'), {
    start: pathStart,
    end: pathStart + '@src/main.cpp '.length,
  });
});

run('clipboard rich text detection is based on clipboard types', () => {
  assert.equal(clipboardHasRichText({ types: ['text/plain'] }), false);
  assert.equal(clipboardHasRichText({ types: ['text/html', 'text/plain'] }), true);
  assert.equal(clipboardHasRichText({ types: ['TEXT/RTF'] }), true);
});

run('plainTextFromClipboardData returns only text/plain and normalizes newlines', () => {
  const clipboardData = {
    types: ['text/html', 'text/plain'],
    getData(type) {
      if (type === 'text/html') return '<b>hello</b>';
      if (type === 'text/plain') return 'hello\r\nworld';
      return '';
    },
  };
  assert.equal(plainTextFromClipboardData(clipboardData), 'hello\nworld');
});

run('rich clipboard without text/plain is detected but contributes no formatted text', () => {
  const clipboardData = {
    types: ['text/html'],
    getData(type) {
      if (type === 'text/html') return '<span style="color:red">styled</span>';
      return '';
    },
  };
  assert.equal(clipboardHasRichText(clipboardData), true);
  assert.equal(plainTextFromClipboardData(clipboardData), '');
});
