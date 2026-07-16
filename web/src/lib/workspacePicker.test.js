import assert from 'node:assert/strict';
import {
  parseWorkspacePickerResult,
  pickExistingWorkspace,
} from './workspacePicker.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`  ✓ ${name}`);
  } catch (error) {
    console.error(`  ✗ ${name}`);
    throw error;
  }
}

await test('parses desktop picker JSON and cancellation results', async () => {
  assert.deepEqual(parseWorkspacePickerResult('{"hash":"abc","cwd":"C:/repo"}'), {
    hash: 'abc',
    cwd: 'C:/repo',
  });
  assert.equal(parseWorkspacePickerResult('null'), null);
  assert.equal(parseWorkspacePickerResult(null), null);
});
await test('desktop picker registers and returns the selected workspace', async () => {
  const calls = [];
  const workspace = await pickExistingWorkspace({
    api: {
      registerWorkspace: async (cwd) => { calls.push(cwd); },
      pickWorkspaceFolder: async () => { throw new Error('unexpected web picker'); },
    },
    desktopBridge: {
      aceDesktop_addWorkspace: async () => JSON.stringify({
        hash: 'workspace-hash',
        cwd: 'C:/existing',
        name: 'existing',
      }),
    },
  });

  assert.equal(workspace.hash, 'workspace-hash');
  assert.deepEqual(calls, ['C:/existing']);
});

await test('web picker uses the existing register-and-return endpoint', async () => {
  const expected = { hash: 'web-hash', cwd: '/home/me/existing', name: 'existing' };
  const workspace = await pickExistingWorkspace({
    api: {
      pickWorkspaceFolder: async () => expected,
      registerWorkspace: async () => { throw new Error('unexpected registration'); },
    },
    desktopBridge: {},
  });
  assert.equal(workspace, expected);
});

await test('picker cancellation preserves a null result', async () => {
  const workspace = await pickExistingWorkspace({
    api: { pickWorkspaceFolder: async () => null },
    desktopBridge: {},
  });
  assert.equal(workspace, null);
});

await test('desktop registration failure does not hide a valid picker result', async () => {
  const workspace = await pickExistingWorkspace({
    api: { registerWorkspace: async () => { throw new Error('already registered'); } },
    desktopBridge: {
      aceDesktop_addWorkspace: async () => ({ hash: 'abc', cwd: 'C:/repo' }),
    },
  });
  assert.equal(workspace.hash, 'abc');
});

await test('visible picker errors are propagated', async () => {
  await assert.rejects(
    pickExistingWorkspace({
      api: { pickWorkspaceFolder: async () => ({ error: 'picker unavailable' }) },
      desktopBridge: {},
    }),
    /picker unavailable/,
  );
});
