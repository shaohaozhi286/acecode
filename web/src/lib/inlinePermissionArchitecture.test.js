import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
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

run('permission cards are chat rows inside the transcript before activity', () => {
  const chat = source('components/ChatView.jsx');
  const transcript = between(
    chat,
    'className="ace-chat-transcript-scroll',
    '<StickyUserContext',
  );
  assert.match(transcript, /permissionRequests\.map/);
  assert.match(transcript, /ace-chat-row-assistant-gutter/);
  assert.match(transcript, /data-chat-kind="permission"/);
  assert.match(transcript, /<PermissionCard/);
  assert.ok(
    transcript.indexOf('permissionRequests.map') < transcript.indexOf('<ActivityIndicator'),
    'permission card must appear before the shared activity bubble',
  );
  assert.match(
    transcript,
    /conversationActivity\.kind !== CONVERSATION_ACTIVITY_KIND\.IDLE/,
  );
  assert.match(chat, /permissionRequests,\s*questionRequest: questionForView/);
});

run('permission geometry participates in tail measurement without changing follow policy', () => {
  const chat = source('components/ChatView.jsx');
  const tailFollowEffect = between(
    chat,
    '// 只在用户仍跟随底部时自动滚到底',
    'useEffect(() => observeChatTailContent',
  );
  assert.match(tailFollowEffect, /permissionRequests/);
  assert.match(tailFollowEffect, /scheduleTailFollowScroll/);
  assert.match(tailFollowEffect, /cancelTailFollowScroll/);
  assert.match(
    chat,
    /\[permissionRequests, scheduleTranscriptMeasures\]/,
  );
  assert.match(chat, /shouldAutoFollowChatTail\(tailFollowStateRef\.current\)/);
});

run('App reconciles server close events and sends decisions to the request session', () => {
  const app = source('App.jsx');
  assert.match(app, /msg\.type === 'permission_request'/);
  assert.match(app, /msg\.type === 'permission_closed'/);
  assert.match(app, /closePermissionRequest\(prev, payload/);
  assert.match(app, /markPermissionSubmitting\(prev, request\.request_id, choice\)/);
  assert.match(
    app,
    /connection\.sendDecision\(request\.request_id, choice, request\.session_id\)/,
  );
  assert.match(app, /clearResolvedPermissionRequests\(prev, sessionId\)/);
  assert.match(app, /payload\.reason === 'permission_timeout'/);
  assert.match(app, /!permissionTimeoutDiagnostic/);
  assert.doesNotMatch(app, /PermissionModal/);
});

run('permission is conversation-scoped and is not a global focus/search/tour blocker', () => {
  const app = source('App.jsx');
  assert.match(app, /visiblePermissionRequests\(permReqs, activeId, permissionOwnership\)/);
  assert.match(app, /const visibleQuestionReq = !visiblePermissionUnresolved/);
  assert.match(app, /permissionOpen: false/);

  const tourBlock = between(app, 'const guidedTourBlocked', 'useEffect(() => initInactiveSelection');
  assert.doesNotMatch(tourBlock, /permReq/);
  const focusBlock = between(
    app,
    'const autoFocusChatOnDesktopWindowFocus',
    'const conversationFindEnabled',
  );
  assert.doesNotMatch(focusBlock, /permReq|visiblePermission/);
});

run('known subagent permission notifications and cards route through the parent', () => {
  const app = source('App.jsx');
  assert.match(app, /subagentDirectory/);
  assert.match(app, /conversationOwnerForSession\(sessionId, payload\)/);
  assert.match(app, /session_id: ownerSessionId \|\| sessionId/);
  assert.match(app, /origin_label: permissionOriginLabel\(entry, permissionOwnership\)/);
});

console.log('inlinePermissionArchitecture tests passed');
