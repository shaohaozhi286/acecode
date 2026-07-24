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

run('conversation turn rail is gated at five projected user turns', () => {
  const helper = source('lib/conversationTurnScrubber.js');
  const chatView = source('components/ChatView.jsx');
  assert.match(helper, /MIN_CONVERSATION_TURN_SCRUBBER_TURNS = 5/);
  assert.match(chatView, /shouldShowConversationTurnScrubber\(\s*preparedConversationTurns/);
  assert.match(chatView, /\{showConversationTurnScrubber && \(/);
});

run('long turn rails use a twenty-marker window with one-step paging and recentering', () => {
  const helper = source('lib/conversationTurnScrubber.js');
  const component = source('components/ConversationTurnScrubber.jsx');
  const styles = source('styles/globals.css');
  assert.match(helper, /MAX_CONVERSATION_TURN_SCRUBBER_MARKERS = 20/);
  assert.match(helper, /export function conversationTurnWindow\(/);
  assert.match(helper, /export function conversationTurnSteppedWindowStart\(/);
  assert.match(helper, /export function conversationTurnWheelDirection\(/);
  assert.match(helper, /export function centeredConversationTurnWindowStart\(/);
  assert.match(helper, /export function conversationTurnPageControlTop\(/);
  assert.match(
    helper,
    /export function conversationTurnWindowStartContainingIndex\(/,
  );
  assert.match(
    component,
    /conversationTurnMarkerLayout\(\s*visibleWindow\.visibleCount,/,
  );
  assert.match(
    component,
    /\{ edgePadding: visibleWindow\.paginated \? 42 : 12 \}/,
  );
  assert.match(helper, /centerOffset = 28/);
  assert.match(
    component,
    /index: visibleWindow\.start \+ marker\.index/,
  );
  assert.match(
    component,
    /setWindowStart\(centeredConversationTurnWindowStart\(/,
  );
  assert.match(component, /data-conversation-turn-page-control="previous"/);
  assert.match(component, /data-conversation-turn-page-control="next"/);
  assert.match(component, /style=\{\{ top: previousPageTop \}\}/);
  assert.match(component, /style=\{\{ top: nextPageTop \}\}/);
  assert.match(component, /onClick=\{\(\) => activatePageControl\(-1\)\}/);
  assert.match(component, /onClick=\{\(\) => activatePageControl\(1\)\}/);
  assert.doesNotMatch(
    styles,
    /data-conversation-turn-page-control="previous"[\s\S]*?top:\s*0/,
  );
  assert.doesNotMatch(
    styles,
    /data-conversation-turn-page-control="next"[\s\S]*?bottom:\s*0/,
  );
  assert.match(
    styles,
    /\.ace-conversation-turn-page-button \{[\s\S]*?top 280ms cubic-bezier\(\.22, 1, \.36, 1\)/,
  );
  assert.match(
    styles,
    /\.ace-conversation-turn-scrubber-hit \{[\s\S]*?transition: top 280ms cubic-bezier\(\.22, 1, \.36, 1\)/,
  );
});

run('paged turn rail applies dominant-axis wheel inertia without transcript scroll', () => {
  const helper = source('lib/conversationTurnScrubber.js');
  const component = source('components/ConversationTurnScrubber.jsx');
  assert.match(helper, /export function conversationTurnWheelImpulse\(/);
  assert.match(component, /const WHEEL_INERTIA_TRANSFER = 0\.2/);
  assert.match(component, /const WHEEL_INERTIA_FRICTION = 0\.84/);
  assert.match(component, /const WHEEL_MIN_STEP_INTERVAL_MS = 72/);
  assert.match(
    component,
    /if \(!rail \|\| !visibleWindow\.paginated\) return undefined;/,
  );
  assert.match(
    component,
    /conversationTurnWheelImpulse\(\s*event\.deltaY,\s*event\.deltaX,\s*event\.deltaMode,/,
  );
  assert.match(component, /if \(event\.ctrlKey \|\| event\.metaKey\) return;/);
  assert.match(
    component,
    /event\.preventDefault\(\);\s*event\.stopPropagation\(\);/,
  );
  assert.match(
    component,
    /currentDirection !== 0 && currentDirection !== direction/,
  );
  assert.match(component, /carry \+= impulse/);
  assert.match(component, /velocity \*= WHEEL_INERTIA_FRICTION/);
  assert.match(
    component,
    /timestamp - lastStepAt >= WHEEL_MIN_STEP_INTERVAL_MS/,
  );
  assert.match(component, /requestAnimationFrame\(advanceWheelMotion\)/);
  assert.match(
    component,
    /addEventListener\('wheel', handleWheel, \{ passive: false \}\)/,
  );
});

run('paging arrow holds repeat with acceleration and suppress the trailing click', () => {
  const helper = source('lib/conversationTurnScrubber.js');
  const component = source('components/ConversationTurnScrubber.jsx');
  assert.match(helper, /export function nextConversationTurnHoldInterval\(/);
  assert.match(component, /const ARROW_HOLD_DELAY_MS = 320/);
  assert.match(
    component,
    /setPointerCapture\?\.\(event\.pointerId\)/,
  );
  assert.match(component, /window\.setTimeout\(repeat, ARROW_HOLD_DELAY_MS\)/);
  assert.match(component, /nextConversationTurnHoldInterval\(hold\.interval\)/);
  assert.match(component, /onPointerDown=\{\(event\) => startArrowHold\(event, -1\)\}/);
  assert.match(component, /onPointerDown=\{\(event\) => startArrowHold\(event, 1\)\}/);
  assert.match(component, /onPointerUp=\{\(\) => stopArrowHold\(true\)\}/);
  assert.match(component, /onPointerCancel=\{\(\) => stopArrowHold\(false\)\}/);
  assert.match(
    component,
    /if \(suppressPageClickRef\.current\) \{\s*suppressPageClickRef\.current = false;\s*return;/,
  );
  assert.match(component, /window\.addEventListener\('blur', cancelHold\)/);
});

run('conversation turn rail loads lazily with an empty failure-safe fallback', () => {
  const chatView = source('components/ChatView.jsx');
  assert.match(
    chatView,
    /lazy\(\s*\(\) => import\('\.\/ConversationTurnScrubber\.jsx'\),?\s*\)/,
  );
  assert.match(chatView, /<Suspense fallback=\{null\}>/);
  assert.match(chatView, /class ConversationTurnScrubberBoundary extends Component/);
  assert.match(chatView, /return this\.state\.failed \? null : this\.props\.children/);
});

run('composer popup host stacks above the rail in legacy WebView2', () => {
  const inputBar = source('components/InputBar.jsx');
  const slashDropdown = source('components/SlashDropdown.jsx');
  const styles = source('styles/globals.css');
  const inputLayer = styles.match(/\.ace-inputbar-layer\s*\{[\s\S]*?z-index:\s*(\d+);[\s\S]*?\}/);
  const scrubberLayer = styles.match(/\.ace-conversation-turn-scrubber\s*\{[\s\S]*?z-index:\s*(\d+);[\s\S]*?\}/);

  assert.match(inputBar, /<div className=\{clsx\(\s*'ace-inputbar-layer',/);
  assert.match(slashDropdown, /className="absolute left-0 right-0 flex flex-col/);
  assert.match(styles, /\.ace-inputbar-layer\s*\{[\s\S]*?position:\s*relative;/);
  assert.ok(inputLayer, 'input-bar stacking rule is missing');
  assert.ok(scrubberLayer, 'conversation-turn scrubber stacking rule is missing');
  assert.ok(
    Number(inputLayer[1]) > Number(scrubberLayer[1]),
    'the input-bar popup host must stack above the conversation-turn scrubber',
  );
});

run('turn preparation waits until transcript paint and idle time and remains cancellable', () => {
  const chatView = source('components/ChatView.jsx');
  assert.match(chatView, /transcriptLoadState !== 'loaded'/);
  assert.match(chatView, /window\.requestAnimationFrame\(\(\) =>/);
  assert.match(chatView, /window\.requestIdleCallback\(prepare, \{ timeout: 450 \}\)/);
  assert.match(chatView, /window\.cancelIdleCallback\(idle\)/);
  assert.match(chatView, /buildConversationTurnPreviews\(itemsRef\.current, \{ busy \}\)/);
  assert.match(chatView, /\[busy, lastUserTurnKey, sid, transcriptLoadState\]/);
});

run('turn activation commits the selected marker before a direct transcript jump', () => {
  const chatView = source('components/ChatView.jsx');
  assert.match(
    chatView,
    /row\.getAttribute\('data-chat-item-id'\) === targetId/,
  );
  const jumpStart = chatView.indexOf('const jumpToConversationTurn');
  const jumpEnd = chatView.indexOf('const focusChatInput', jumpStart);
  const jumpSource = chatView.slice(jumpStart, jumpEnd);
  assert.match(jumpSource, /pauseTailFollowForReview\(\)/);
  assert.match(
    jumpSource,
    /flushSync\(\(\) => \{\s*setActiveConversationTurn\(index\);\s*\}\)/,
  );
  assert.match(jumpSource, /el\.scrollTop = targetScrollTop/);
  assert.match(
    jumpSource,
    /conversationTurnActivationRef\.current = \{\s*sid,\s*itemId: targetId,\s*scrollTop: el\.scrollTop,\s*\}/,
  );
  assert.doesNotMatch(jumpSource, /behavior:\s*['"]smooth['"]/);
  assert.ok(
    jumpSource.indexOf('setActiveConversationTurn(index)')
      < jumpSource.indexOf('el.scrollTop = targetScrollTop'),
  );
  assert.match(chatView, /data-chat-item-id=\{String\(it\.id\)\}/);
});

run('explicit marker activation survives clamped remeasurement until scroll changes', () => {
  const helper = source('lib/conversationTurnScrubber.js');
  const chatView = source('components/ChatView.jsx');
  assert.match(helper, /export function activatedConversationTurnIndex\(/);
  assert.match(chatView, /const conversationTurnActivationRef = useRef\(null\)/);
  assert.match(
    chatView,
    /resolveActivatedConversationTurnIndex\(\s*turnsForRail,\s*activation,\s*el\.scrollTop,\s*\)/,
  );
  assert.match(
    chatView,
    /if \(activatedIndex >= 0\) \{[\s\S]*?return;\s*\}\s*conversationTurnActivationRef\.current = null;/,
  );
});

run('scrubber markers keep hover-only previews with axis-aligned spring displacement', () => {
  const component = source('components/ConversationTurnScrubber.jsx');
  const styles = source('styles/globals.css');
  assert.match(component, /onPointerMove=\{selectFromPointer\}/);
  assert.match(
    component,
    /const previewIndex = indexIsVisible\(hoveredIndex\) \? hoveredIndex : -1;/,
  );
  assert.match(
    component,
    /focusedIndex >= 0 \? focusedIndex : activeIndex/,
  );
  assert.match(component, /pointerFocusIndexRef/);
  assert.match(component, /conversationTurnMarkerDisplacement\(/);
  assert.match(component, /onClick=\{\(\) => jumpToTurn\(turn, marker\.index\)\}/);
  assert.match(component, /event\.key !== 'Enter' && event\.key !== ' '/);
  assert.doesNotMatch(component, />提问</);
  assert.doesNotMatch(component, />回答</);
  assert.doesNotMatch(styles, /grid-template-columns:\s*34px/);
  assert.match(
    styles,
    /transform: translate3d\(0, var\(--ace-conversation-turn-marker-shift-y\), 0\) scale\(2\)/,
  );
  assert.match(styles, /transform 420ms cubic-bezier\(\.34, 1\.56, \.64, 1\)/);
  assert.match(
    styles,
    /linear\(0, \.72 20%, 1\.08 42%, \.97 62%, 1\.015 78%, 1\)/,
  );
  assert.doesNotMatch(styles, /translate3d\(4px, 0, 0\) scale\(2\)/);
  assert.match(styles, /@media \(prefers-reduced-motion: reduce\)/);
  assert.doesNotMatch(
    styles,
    /@media \(prefers-reduced-motion: reduce\) \{\s*\.ace-conversation-turn-scrubber-dot/,
  );
  assert.match(
    styles,
    /@media \(prefers-reduced-motion: reduce\) \{\s*\.ace-conversation-turn-preview \{\s*transition: none;/,
  );
});
