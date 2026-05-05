import assert from 'node:assert/strict';
import { renderMarkdown } from './markdown.js';
import {
  CODE_COPY_BUTTON_SELECTOR,
  CODE_COPY_FRAME_SELECTOR,
  CODE_COPY_SOURCE_SELECTOR,
  codeTextFromCopyButtonTarget,
  codeTextFromFrame,
  copyTextToClipboard,
} from './codeBlockCopy.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('markdown fenced code block renders copyable frame', () => {
  const html = renderMarkdown('```js\nconst value = "<tag>";\n```');
  assert.match(html, /data-code-copy-frame="true"/);
  assert.match(html, /data-code-copy-button="true"/);
  assert.match(html, /language-javascript/);
  assert.doesNotMatch(html, /```/);
  assert.doesNotMatch(html, /<tag>/);
  assert.match(html, /&lt;tag&gt;/);
});

await run('markdown inline code stays inline without copy frame', () => {
  const html = renderMarkdown('Use `const value = 1` inline.');
  assert.match(html, /<code>const value = 1<\/code>/);
  assert.doesNotMatch(html, /data-code-copy-frame="true"/);
  assert.doesNotMatch(html, /data-code-copy-button="true"/);
});

await run('copy text extraction reads only code source text', () => {
  const frame = {
    querySelector(selector) {
      if (selector === CODE_COPY_SOURCE_SELECTOR) return { textContent: 'echo hello\n' };
      throw new Error(`unexpected selector: ${selector}`);
    },
  };
  assert.equal(codeTextFromFrame(frame), 'echo hello\n');
});

await run('copy button target resolves owning code frame', () => {
  const frame = {
    querySelector(selector) {
      if (selector === CODE_COPY_SOURCE_SELECTOR) return { textContent: 'npm test' };
      return null;
    },
  };
  const button = {
    closest(selector) {
      return selector === CODE_COPY_FRAME_SELECTOR ? frame : null;
    },
  };
  const target = {
    closest(selector) {
      return selector === CODE_COPY_BUTTON_SELECTOR ? button : null;
    },
  };
  assert.equal(codeTextFromCopyButtonTarget(target), 'npm test');
});

await run('copy button resolves from action slot beside wrap button', () => {
  const frame = {
    querySelector(selector) {
      if (selector === CODE_COPY_SOURCE_SELECTOR) return { textContent: 'long line without visual changes' };
      return null;
    },
  };
  const actionBar = {
    closest(selector) {
      return selector === CODE_COPY_FRAME_SELECTOR ? frame : null;
    },
  };
  const copyButton = {
    closest(selector) {
      if (selector === CODE_COPY_FRAME_SELECTOR) return frame;
      if (selector === '[data-code-actions="true"]') return actionBar;
      return null;
    },
  };
  const target = {
    closest(selector) {
      return selector === CODE_COPY_BUTTON_SELECTOR ? copyButton : null;
    },
  };
  assert.equal(codeTextFromCopyButtonTarget(target), 'long line without visual changes');
});

await run('wrap action button is not treated as copy target', () => {
  const wrapButtonTarget = { closest: () => null };
  assert.equal(codeTextFromCopyButtonTarget(wrapButtonTarget), null);
});

await run('wrapped preview copy remains source accurate', () => {
  const source = 'alpha beta gamma delta epsilon\nindented    value';
  const frame = {
    dataset: { wrap: 'true' },
    querySelector(selector) {
      if (selector === CODE_COPY_SOURCE_SELECTOR) return { textContent: source };
      return null;
    },
  };
  assert.equal(codeTextFromFrame(frame), source);
});

await run('non-copy targets are ignored', () => {
  const inlineCodeTarget = { closest: () => null };
  assert.equal(codeTextFromCopyButtonTarget(inlineCodeTarget), null);
});

await run('clipboard copy succeeds with raw text', async () => {
  const writes = [];
  const copied = await copyTextToClipboard('line 1\nline 2', {
    writeText: async (text) => { writes.push(text); },
  });
  assert.equal(copied, 'line 1\nline 2');
  assert.deepEqual(writes, ['line 1\nline 2']);
});

await run('clipboard copy failure is surfaced', async () => {
  await assert.rejects(
    () => copyTextToClipboard('x', { writeText: async () => { throw new Error('denied'); } }),
    /denied/,
  );
});
