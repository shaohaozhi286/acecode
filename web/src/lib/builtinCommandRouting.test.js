import assert from 'node:assert/strict';
import {
  builtinCommandRequestForText,
  inputRouteForText,
  sideQuestionRequestForText,
  sessionCreateOptionsForText,
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
    question: 'explain this\nplease',
    display_text: '/BTW   explain this\nplease',
  });
  assert.deepEqual(inputRouteForText('/btw why?'), {
    kind: 'side_question',
    question: 'why?',
    display_text: '/btw why?',
  });
  assert.deepEqual(inputRouteForText('/btw'), {
    kind: 'side_question',
    question: '',
    display_text: '/btw',
  });
  assert.equal(sideQuestionRequestForText('/btween no'), null);
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
