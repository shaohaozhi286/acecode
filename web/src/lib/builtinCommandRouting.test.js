import assert from 'node:assert/strict';
import {
  builtinCommandRequestForText,
  inputRouteForText,
  sideQuestionRequestForText,
  sessionCreateOptionsForText,
  turnSteerRequestForText,
} from './builtinCommandRouting.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('builtin slash input routes to command endpoint payload', () => {
  assert.deepEqual(builtinCommandRequestForText('/init scan this repo'), {
    command: 'init',
    args: 'scan this repo',
    display_text: '/init scan this repo',
  });
  assert.deepEqual(inputRouteForText('/compact'), {
    kind: 'builtin',
    command: {
      command: 'compact',
      args: '',
      display_text: '/compact',
    },
  });
  assert.deepEqual(inputRouteForText('/plan inspect first'), {
    kind: 'builtin',
    command: {
      command: 'plan',
      args: 'inspect first',
      display_text: '/plan inspect first',
    },
  });
});

run('skill slash input remains ordinary message route', () => {
  assert.deepEqual(inputRouteForText('/code-review check this'), {
    kind: 'message',
    text: '/code-review check this',
  });
});

run('/btw routes immediately as a side question before builtin parsing', () => {
  assert.deepEqual(sideQuestionRequestForText('  /BTW   explain this\nplease  '), {
    command: 'btw',
    question: 'explain this\nplease',
    display_text: '/BTW   explain this\nplease',
  });
  assert.deepEqual(inputRouteForText('/btw why?'), {
    kind: 'side_question',
    command: 'btw',
    question: 'why?',
    display_text: '/btw why?',
  });
  assert.deepEqual(inputRouteForText('/btw'), {
    kind: 'side_question',
    command: 'btw',
    question: '',
    display_text: '/btw',
  });
  assert.deepEqual(inputRouteForText('/side why?'), {
    kind: 'side_question',
    command: 'side',
    question: 'why?',
    display_text: '/side why?',
  });
  assert.equal(sideQuestionRequestForText('/btween no'), null);
});

run('/turn routes to active-turn steering instead of a builtin or ordinary message', () => {
  assert.deepEqual(turnSteerRequestForText(' /TURN  use the new constraint '), {
    guidance: 'use the new constraint',
    display_text: '/TURN  use the new constraint',
  });
  assert.deepEqual(inputRouteForText('/turn keep the API stable'), {
    kind: 'turn_steer',
    guidance: 'keep the API stable',
    display_text: '/turn keep the API stable',
  });
  assert.equal(turnSteerRequestForText('/turnip no'), null);
});

run('unknown slash input remains ordinary message route', () => {
  assert.deepEqual(inputRouteForText('/foobar test'), {
    kind: 'message',
    text: '/foobar test',
  });
});

run('home builtin session creation disables auto start', () => {
  assert.deepEqual(sessionCreateOptionsForText('/init'), {
    auto_start: false,
  });
  assert.deepEqual(sessionCreateOptionsForText('/btw quick question'), {
    auto_start: false,
  });
  assert.deepEqual(sessionCreateOptionsForText('/side quick question'), {
    auto_start: false,
  });
  assert.deepEqual(sessionCreateOptionsForText('/turn guide this'), {
    auto_start: false,
  });
});

run('home ordinary message session creation auto starts text', () => {
  assert.deepEqual(sessionCreateOptionsForText('/code-review check this'), {
    initial_user_message: '/code-review check this',
    auto_start: true,
  });
  assert.deepEqual(sessionCreateOptionsForText('hello'), {
    initial_user_message: 'hello',
    auto_start: true,
  });
});
