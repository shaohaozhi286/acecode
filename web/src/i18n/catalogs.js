import { enUS } from './catalogs/en-US.js';
import { zhCN } from './catalogs/zh-CN.js';
import { sourceCatalogs } from './sourceCatalog.generated.js';

export const translationCatalogs = Object.freeze({
  'zh-CN': { ...zhCN, source: sourceCatalogs['zh-CN'] },
  'en-US': { ...enUS, source: sourceCatalogs['en-US'] },
});
