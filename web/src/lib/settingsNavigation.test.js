import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  SETTINGS_NAV_GROUPS,
  SETTINGS_NAV_ITEMS,
  settingsNavIndexForKey,
} from './settingsNavigation.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('settings navigation uses the confirmed Codex-style groups', () => {
  assert.deepEqual(
    SETTINGS_NAV_GROUPS.map((group) => ({
      key: group.key,
      label: group.label,
      items: group.items.map((item) => item.label),
    })),
    [
      {
        key: 'personal',
        label: '个人',
        items: ['常规', '外观', '配置', '个性化', '使用情况'],
      },
      {
        key: 'integrations',
        label: '集成',
        items: ['技能', 'MCP 服务器', '连接器'],
      },
      {
        key: 'coding',
        label: '编码',
        items: ['模型', '工具', '钩子'],
      },
      {
        key: 'archived',
        label: '已归档',
        items: ['已归档会话'],
      },
      {
        key: 'support',
        label: '支持',
        items: ['问题反馈', '关于'],
      },
    ],
  );
});

test('flattened settings routes remain unique and complete', () => {
  const keys = SETTINGS_NAV_ITEMS.map((item) => item.key);
  assert.deepEqual(keys, [
    'general',
    'appearance',
    'config',
    'personalization',
    'usage',
    'skills',
    'mcp',
    'connectors',
    'models',
    'tools',
    'hooks',
    'archived',
    'feedback',
    'about',
  ]);
  assert.equal(new Set(keys).size, keys.length);
});

test('settings deep links use grouped indexes and fall back to general', () => {
  SETTINGS_NAV_ITEMS.forEach((item, index) => {
    assert.equal(settingsNavIndexForKey(item.key), index);
  });
  assert.equal(settingsNavIndexForKey('missing-section'), 0);
  assert.equal(settingsNavIndexForKey(''), 0);
});

test('SettingsPage renders accessible groups inside the scrollable navigation', () => {
  const source = readFileSync(
    new URL('../components/SettingsPage.jsx', import.meta.url),
    'utf8',
  );
  assert.match(source, /SETTINGS_NAV_GROUPS\.map/);
  assert.match(source, /role="group"/);
  assert.match(source, /aria-labelledby=\{headingId\}/);
  assert.match(source, /aria-current=\{active \? 'page' : undefined\}/);
  assert.match(source, /<nav className="[^"]*overflow-y-auto/);
  assert.match(source, /text-fg-mute opacity-75/);
});
