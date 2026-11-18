// 夹具：缓存用例的中间模块（与 entry 同时导入 './counter.js'，解析结果必须同一个模块名）。
import { A as inner } from './counter.js';

export const B = 'wrapped:' + inner;
