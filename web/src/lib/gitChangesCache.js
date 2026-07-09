// git 级变更的**共享**缓存实例(单例)。
//
// SidePanel 的 `GitChangesPanel`(紧凑导航列表)与中间详情栏的
// `GitChangeReview`(可展开 diff 的审查视图)都读同一份缓存:导航列表拉过
// numstat / 单文件 patch 后,详情栏直接命中,不重复打后端。失效(markStale)
// 也由这一份实例统一收口 —— 回合结束 / checkout / 手动刷新任一处标脏,两个
// 视图下次读取都会重拉。
//
// 之前 GitChangesPanel 自己 `createChangesCache()` 一个模块级实例,详情栏若
// 各建一份就会双份网络请求且失效不同步,故抽到这里共享。

import { createChangesCache } from './gitChanges.js';

export const changesCache = createChangesCache();
