import assert from 'node:assert/strict';
import { todoChecklistPresentation } from './todoChecklist.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('todo checklist presentation hides empty lists', () => {
  const checklist = todoChecklistPresentation([]);
  assert.equal(checklist.visible, false);
  assert.equal(checklist.total, 0);
  assert.deepEqual(checklist.items, []);
});

run('todo checklist presentation maps visible status markers', () => {
  const checklist = todoChecklistPresentation([
    { id: 'p', content: 'Plan', status: 'pending' },
    { id: 'a', content: 'Apply', status: 'in_progress' },
    { id: 'd', content: 'Done', status: 'completed' },
    { id: 'c', content: 'Cancelled', status: 'cancelled' },
    { id: 'x', content: '', status: 'unknown' },
  ]);

  assert.equal(checklist.visible, true);
  assert.equal(checklist.done, 1);
  assert.equal(checklist.total, 5);
  assert.deepEqual(checklist.items.map((item) => item.status), [
    'pending',
    'in_progress',
    'completed',
    'cancelled',
    'pending',
  ]);
  assert.deepEqual(checklist.items.map((item) => item.icon), ['none', 'dot', 'check', 'dash', 'none']);
  assert.equal(checklist.items[1].markerLabel, 'active');
  assert.match(checklist.items[2].textClassName, /line-through/);
  assert.match(checklist.items[1].textClassName, /font-medium/);
  assert.equal(checklist.items[4].content, '(no description)');
});

run('todo checklist presentation prefers valid summary counts', () => {
  const checklist = todoChecklistPresentation([
    { id: 'p', content: 'Plan', status: 'pending' },
    { id: 'd', content: 'Done', status: 'completed' },
  ], { completed: 4, total: 7 });

  assert.equal(checklist.done, 4);
  assert.equal(checklist.total, 7);
});
