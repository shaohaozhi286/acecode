import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('composer 附近不再存在重复的固定活动栏', () => {
  const chat = source('components/ChatView.jsx');
  const input = source('components/InputBar.jsx');
  assert.doesNotMatch(chat, /ConversationActivityRail|connectedTop/);
  assert.equal(
    fs.existsSync(path.join(srcRoot, 'components/ConversationActivityRail.jsx')),
    false,
  );
  assert.match(input, /'border-t border-border px-2\.5 py-2 bg-surface shrink-0'/);
});

run('会话族状态只通过 transcript 尾部原有气泡呈现', () => {
  const chat = source('components/ChatView.jsx');
  assert.match(chat, /selectConversationActivity\(\{/);
  assert.match(
    chat,
    /permissionRequests\.map[\s\S]*?conversationActivity\.kind !== CONVERSATION_ACTIVITY_KIND\.IDLE[\s\S]*?<ActivityIndicator[\s\S]*?showConversationTurnScrubber/,
  );
  assert.match(chat, /data-conversation-activity-bubble="true"/);
});

run('气泡状态不再由旧的文本或工具可见性条件漏掉', () => {
  const chat = source('components/ChatView.jsx');
  const start = chat.indexOf('{conversationActivity.kind !== CONVERSATION_ACTIVITY_KIND.IDLE');
  const end = chat.indexOf('showConversationTurnScrubber', start);
  const mount = chat.slice(start, end);
  assert.ok(start >= 0 && end > start);
  assert.doesNotMatch(
    mount,
    /hasVisibleStreamingAssistant|hasActiveTool|permissionRequests\.length/,
  );
});

run('ActivityIndicator 保持流内气泡视觉且包含恢复和后台状态', () => {
  const chat = source('components/ChatView.jsx');
  const selector = source('lib/conversationActivity.js');
  const start = chat.indexOf('function ActivityIndicator');
  const end = chat.indexOf('function ActivitySummaryBlock', start);
  const indicator = chat.slice(start, end);
  assert.ok(start >= 0 && end > start);
  assert.match(indicator, /border-border bg-surface-hi/);
  assert.doesNotMatch(indicator, /\bfixed\b|\bsticky\b|\babsolute\b|bottom-|inset-/);
  assert.doesNotMatch(indicator, /#[0-9a-fA-F]{3,8}/);
  assert.match(selector, /正在恢复权限请求/);
  assert.match(selector, /正在恢复提问请求/);
  assert.match(selector, /主会话仍可继续输入/);
});

console.log('conversationLivenessArchitecture tests passed');
