import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const component = fs.readFileSync(
  path.join(here, '..', 'components', 'SlashDropdown.jsx'),
  'utf8',
);

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('slash dropdown applies measured anchored placement before paint', () => {
  assert.match(component, /useLayoutEffect\(\(\) => \{\s*measureLayout\(\);/);
  assert.match(component, /computeAnchoredDropdownLayout\(\{/);
  assert.match(component, /anchorTop:\s*anchorRect\.top/);
  assert.match(component, /anchorBottom:\s*anchorRect\.bottom/);
  assert.match(component, /data-placement=\{layout\.placement\}/);
  assert.match(component, /top:\s*opensBelow \?/);
  assert.match(component, /bottom:\s*opensBelow \?/);
  assert.match(component, /maxHeight:\s*layout\.maxHeight/);
});

run('slash dropdown follows window visual viewport and anchor resizing', () => {
  assert.match(component, /window\.addEventListener\('resize', measureLayout\)/);
  assert.match(component, /window\.addEventListener\('scroll', measureLayout, true\)/);
  assert.match(component, /visualViewport\?\.addEventListener\?\.\('resize', measureLayout\)/);
  assert.match(component, /visualViewport\?\.addEventListener\?\.\('scroll', measureLayout\)/);
  assert.match(component, /new ResizeObserver\(measureLayout\)/);
  assert.match(component, /resizeObserver\.observe\(anchor\)/);
  assert.match(component, /resizeObserver\.observe\(listRef\.current\)/);
});

run('constrained menu shrinks its scrolling list inside the outer max height', () => {
  assert.match(component, /className="absolute left-0 right-0 flex flex-col/);
  assert.match(component, /className="min-h-0 flex-1 overflow-y-auto"/);
  assert.match(component, /clientHeight:\s*list\.clientHeight/);
  assert.match(component, /visibleEnd = scrollMetrics\.clientHeight > 0/);
  assert.match(component, /onScroll=\{updateScrollMetrics\}/);
});
