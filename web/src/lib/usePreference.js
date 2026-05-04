// 统一的 UI 偏好读写 hook,所有 localStorage 偏好读写应走此处。
//
// 用法:
//   const [prefs, setPrefs] = usePreference('acecode.uiPrefs.v1',
//     { view: 'single', sidePanelCollapsed: false }, validateUiPrefs);
//   setPrefs({ view: 'grid4' });            // partial 浅合并
//   setPrefs(p => ({ ...p, sidePanelCollapsed: !p.sidePanelCollapsed }));
//
// 行为:
//   - 初次 mount 从 storage 读;失败 / JSON parse 错 / validator 拒 → defaults
//   - setter partial 与 prev 浅合并;非对象 / null 整体替换
//   - 写 storage 失败(隐私模式 / quota)静默吞,内存 state 仍按用户操作更新
//
// 约束:同一 storageKey 在整个 App 内只允许一个 hook 实例订阅 — 否则两个实例
// 各自的 useState 不共享,会因后写覆盖出现 last-write-wins 错乱。

import { useCallback, useEffect, useState } from 'react';

function defaultStorage() {
  return typeof globalThis !== 'undefined' ? globalThis.localStorage : undefined;
}

export function readWithFallback(key, defaults, validator, storage = defaultStorage()) {
  if (!storage) return defaults;
  let raw;
  try { raw = storage.getItem(key); } catch { return defaults; }
  if (raw == null) return defaults;
  let parsed;
  try { parsed = JSON.parse(raw); } catch { return defaults; }
  if (typeof validator === 'function' && !validator(parsed)) return defaults;
  return parsed;
}

export function writeSafely(key, value, storage = defaultStorage()) {
  if (!storage) return false;
  try { storage.setItem(key, JSON.stringify(value)); return true; }
  catch { return false; }
}

// setter 调用时把 updater 折叠到下一轮 value:
//   - 函数 → updater(prev)
//   - 对象 → 与 prev 浅合并(prev 也必须是对象;否则整体替换)
//   - 其他(string/number/boolean/null)→ 整体替换
export function mergeNextValue(prev, updater) {
  if (typeof updater === 'function') return updater(prev);
  if (updater && typeof updater === 'object' && !Array.isArray(updater)
      && prev && typeof prev === 'object' && !Array.isArray(prev)) {
    return { ...prev, ...updater };
  }
  return updater;
}

export function usePreference(storageKey, defaults, validator) {
  const [value, setValue] = useState(() =>
    readWithFallback(storageKey, defaults, validator));

  useEffect(() => {
    writeSafely(storageKey, value);
    // storageKey 变了走重新订阅是无意义场景;此处只跟踪 value
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [value]);

  const update = useCallback((updater) => {
    setValue((prev) => mergeNextValue(prev, updater));
  }, []);

  return [value, update];
}
