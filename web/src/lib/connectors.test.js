// normalizeConnectorList / applyConnectorToggle 单元测试。
//
// 回归目标:设置页「连接器」开关不能把 config.json 里的 hooks / auth_error_scope
// 冲掉。开关流程是 GET -> normalizeConnectorList -> (点击) applyConnectorToggle
// -> PUT { connectors: next } -> normalizeConnectorList(result)。任何一步把
// hooks/auth_error_scope 之类的字段丢了,PUT 回去的 config.json 就永久清空了
// on_enable 钩子。

import assert from 'node:assert/strict';
import { normalizeConnectorList, applyConnectorToggle } from './connectors.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const HOOKS = {
  on_enable: { command: 'bash', args: ['./on-enable.sh'], timeout_ms: 5000 },
};
const AUTH_ERROR_SCOPE = { base_url_prefix: 'https://example.test/api/' };

run('normalizeConnectorList 保留 hooks / auth_error_scope / 未知字段', () => {
  const data = {
    connectors: [
      {
        id: 'c1',
        name: 'Example Connector',
        description: 'desc',
        enabled: true,
        hooks: HOOKS,
        auth_error_scope: AUTH_ERROR_SCOPE,
        future_field: { nested: 'value' },
      },
    ],
  };
  const list = normalizeConnectorList(data);
  assert.equal(list.length, 1);
  const item = list[0];
  assert.equal(item.id, 'c1');
  assert.equal(item.name, 'Example Connector');
  assert.equal(item.enabled, true);
  // 核心回归点:hooks/auth_error_scope 必须原样保留(深相等,不是引用相等就行)
  assert.deepEqual(item.hooks, HOOKS);
  assert.deepEqual(item.auth_error_scope, AUTH_ERROR_SCOPE);
  // 未知字段(面向未来)也要透传
  assert.deepEqual(item.future_field, { nested: 'value' });
});

run('normalizeConnectorList 清洗已知字段的类型/默认值', () => {
  const list = normalizeConnectorList({
    connectors: [
      { id: '  c2  ', name: 123, description: null, enabled: 'yes' },
    ],
  });
  assert.equal(list.length, 1);
  assert.equal(list[0].id, 'c2'); // trim
  assert.equal(list[0].name, ''); // 非字符串兜底空串
  assert.equal(list[0].description, '');
  assert.equal(list[0].enabled, true); // 真值转 boolean
});

run('normalizeConnectorList 过滤掉没有 id 的条目,容忍非法输入', () => {
  assert.deepEqual(normalizeConnectorList(null), []);
  assert.deepEqual(normalizeConnectorList({}), []);
  assert.deepEqual(normalizeConnectorList({ connectors: 'nope' }), []);
  assert.deepEqual(normalizeConnectorList({ connectors: [null, 42, { name: 'no id' }] }), []);
});

run('applyConnectorToggle 只翻转目标 connector 的 enabled,保留其余字段', () => {
  const src = [
    { id: 'c1', name: 'A', description: '', enabled: false, hooks: HOOKS, auth_error_scope: AUTH_ERROR_SCOPE },
    { id: 'c2', name: 'B', description: '', enabled: true },
  ];
  const next = applyConnectorToggle(src, 'c1', true);
  assert.equal(next[0].enabled, true);
  assert.deepEqual(next[0].hooks, HOOKS);
  assert.deepEqual(next[0].auth_error_scope, AUTH_ERROR_SCOPE);
  // 未涉及的 connector 原样保留(同引用也可以,值相等即可)
  assert.deepEqual(next[1], src[1]);
  // 源数组 / 源对象不被 mutate(开关失败时前端要能回滚)
  assert.equal(src[0].enabled, false);
});

run('applyConnectorToggle 找不到目标 id 时列表不变(值相等),容忍非法输入', () => {
  const src = [{ id: 'c1', name: 'A', description: '', enabled: false }];
  const next = applyConnectorToggle(src, 'missing', true);
  assert.deepEqual(next, src);
  assert.deepEqual(applyConnectorToggle(null, 'c1', true), []);
  assert.deepEqual(applyConnectorToggle('nope', 'c1', true), []);
});

run('端到端:normalize -> toggle -> normalize(模拟 PUT 往返)hooks 全须存活', () => {
  const getResponse = {
    connectors: [
      { id: 'c1', name: 'A', description: '', enabled: false, hooks: HOOKS, auth_error_scope: AUTH_ERROR_SCOPE },
    ],
  };
  const loaded = normalizeConnectorList(getResponse);
  const toggled = applyConnectorToggle(loaded, 'c1', true);
  // 模拟 PUT 请求体 { connectors: toggled },以及服务端把同一份数据原样返回
  const putResponse = { connectors: toggled };
  const afterPut = normalizeConnectorList(putResponse);
  assert.equal(afterPut[0].enabled, true);
  assert.deepEqual(afterPut[0].hooks, HOOKS);
  assert.deepEqual(afterPut[0].auth_error_scope, AUTH_ERROR_SCOPE);
});
