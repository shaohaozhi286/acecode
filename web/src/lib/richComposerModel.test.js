import assert from 'node:assert/strict';
import { flattenCommands } from './slashCommands.js';
import {
  clipboardHasRichText,
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

run('unknown leading command remains plain text', () => {
  const text = '/not-known investigate input';
  const model = richComposerModelFromText(text, COMMANDS);
  assert.equal(model.kind, 'plain');
  assert.equal(model.command, null);
  assert.equal(richComposerTextFromModel(model), text);
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
