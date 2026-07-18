import assert from 'node:assert/strict';
import {
  archivedSessionKey,
  archivedSessionTarget,
  removeArchivedSessionsByKey,
  selectedArchivedSessions,
  shouldToggleArchivedSessionRow,
} from './archivedSessions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('archived session identity includes workspace and accepts legacy field names', () => {
  assert.deepEqual(
    archivedSessionTarget({ session_id: 'session-a', workspaceHash: 'workspace-1' }),
    {
      id: 'session-a',
      workspaceHash: 'workspace-1',
      key: '["workspace-1","session-a"]',
    },
  );
  assert.equal(
    archivedSessionKey({ id: 'session-a' }),
    '["__local__","session-a"]',
  );
});

test('same session id in different workspaces remains independently selectable', () => {
  const items = [
    { id: 'same-id', workspace_hash: 'workspace-1' },
    { id: 'same-id', workspace_hash: 'workspace-2' },
  ];
  const selected = new Set([archivedSessionKey(items[1])]);
  assert.deepEqual(selectedArchivedSessions(items, selected), [items[1]]);
  assert.deepEqual(removeArchivedSessionsByKey(items, selected), [items[0]]);
});

test('invalid archived rows do not participate in selection or removal', () => {
  const invalid = { workspace_hash: 'workspace-1' };
  assert.equal(archivedSessionKey(invalid), '');
  assert.deepEqual(selectedArchivedSessions([invalid], new Set([''])), []);
  assert.deepEqual(removeArchivedSessionsByKey([invalid], new Set([''])), [invalid]);
});

test('archived row toggles outside interactive controls', () => {
  const target = (interactiveMatch) => ({
    closest: () => interactiveMatch,
  });

  assert.equal(shouldToggleArchivedSessionRow(target(null)), true);
  assert.equal(shouldToggleArchivedSessionRow(target({ tagName: 'BUTTON' })), false);
  assert.equal(shouldToggleArchivedSessionRow(target({ tagName: 'INPUT' })), false);
  assert.equal(shouldToggleArchivedSessionRow(null), true);
});
