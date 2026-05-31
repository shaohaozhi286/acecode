import assert from 'node:assert/strict';
import {
  attachmentsFromContentParts,
  contextsFromContentParts,
  isImageAttachment,
  normalizeAttachmentList,
} from './messageAttachments.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('content parts expose image and file attachments', () => {
  const parts = [
    { type: 'text', text: 'hello' },
    { type: 'image', attachment: { id: 'att-img', name: 'screen.png', mime_type: 'image/png' } },
    { type: 'file', attachment: { id: 'att-file', name: 'report.txt', mime_type: 'text/plain' } },
    { type: 'browser_context', context: { id: 'ctx', label: 'Browser' } },
  ];
  assert.deepEqual(
    attachmentsFromContentParts(parts).map((att) => [att.id, att.type]),
    [['att-img', 'image'], ['att-file', 'file']],
  );
  assert.deepEqual(contextsFromContentParts(parts), [{ id: 'ctx', label: 'Browser' }]);
});

run('attachment normalization accepts records and content part wrappers', () => {
  const list = normalizeAttachmentList([
    { id: 'a', name: 'a.png', kind: 'image' },
    { type: 'image', attachment: { id: 'b', name: 'b.jpg', mime_type: 'image/jpeg' } },
  ]);
  assert.equal(list.length, 2);
  assert.equal(list[1].id, 'b');
  assert.equal(isImageAttachment(list[0]), true);
  assert.equal(isImageAttachment({ name: 'x.txt', mime_type: 'text/plain' }), false);
});
