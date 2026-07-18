import assert from 'node:assert/strict';
import { loadTier, loadTierTextClass, pickModelLoad } from './modelLoad.js';

function run(name, fn) {
  try {
    fn();
    console.log(`  ok  modelLoad > ${name}`);
  } catch (e) {
    console.error(`  FAIL modelLoad > ${name}`);
    throw e;
  }
}

// 场景:负载色阶分档。阈值由用户定义,边界逐个钉死,与后端 model_load_tier 一致。
// 期望:<70 绿 / 70..90 黄 / >90 红 / 非法→null。图2 的 93% 落红。
run('loadTier thresholds', () => {
  assert.equal(loadTier(0), 'green');
  assert.equal(loadTier(69), 'green');
  assert.equal(loadTier(70), 'yellow'); // 70 不算 "70以下"
  assert.equal(loadTier(90), 'yellow'); // "90以下" 含 90
  assert.equal(loadTier(91), 'red');
  assert.equal(loadTier(93), 'red');
  assert.equal(loadTier(100), 'red');
  assert.equal(loadTier(-1), null);
  assert.equal(loadTier(NaN), null);
  assert.equal(loadTier('80'), null); // 非 number
});

// 场景:色阶映射到文本色类。期望:红/黄/绿/兜底各自对应 globals.css token。
run('loadTierTextClass mapping', () => {
  assert.equal(loadTierTextClass('red'), 'text-danger');
  assert.equal(loadTierTextClass('yellow'), 'text-warn');
  assert.equal(loadTierTextClass('green'), 'text-ok');
  assert.equal(loadTierTextClass(null), 'text-fg-mute');
});

// 场景:从快照按模型 id 精确匹配取负载。
// 期望:命中返回归一化对象;模型 id 必须精确等于 modelPoolName。
// 回归点:modelPoolName 无需 PUB 前缀;未命中 / 大小写不同 / 空 id → null。
run('pickModelLoad exact match', () => {
  const models = [
    { modelPoolName: 'DeepSeek-V4-Flash', usageRate: 60, maxWindowTokens: 150000, effectiveContextWindow: 120000 },
    { modelPoolName: 'PUB-Qwen3.6-35B-A3B-FP8', usageRate: 93, maxWindowTokens: 150000, effectiveContextWindow: 120000 },
  ];
  const hit = pickModelLoad(models, 'DeepSeek-V4-Flash');
  assert.equal(hit.usageRate, 60);
  assert.equal(hit.effectiveContextWindow, 120000);

  assert.equal(pickModelLoad(models, 'gpt-4o'), null); // 未出现在 modelPoolName 中
  assert.equal(pickModelLoad(models, 'deepseek-v4-flash'), null); // 大小写不同
  assert.equal(pickModelLoad(models, 'PUB-Unknown'), null); // 只有前缀也不算命中
  assert.equal(pickModelLoad(models, ''), null);
  assert.equal(pickModelLoad(models, null), null);
  assert.equal(pickModelLoad(null, 'DeepSeek-V4-Flash'), null);
});

// 场景:命中池但 usageRate 缺失/无效。期望:返回 null(不显示一个未知负载)。
run('pickModelLoad hit but unknown usage → null', () => {
  const models = [{ modelPoolName: 'PUB-X', maxWindowTokens: 150000 }];
  assert.equal(pickModelLoad(models, 'PUB-X'), null);
});
