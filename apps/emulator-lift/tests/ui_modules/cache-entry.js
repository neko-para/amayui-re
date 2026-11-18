// 夹具：同一模块被两条路径导入（直接 + 经 kit-cache）—— 判据是"只求值一次"（缓存）。
import { A } from './counter.js';
import { B } from './kit-cache.js';

globalThis.render = function render() {
  ui.text({ x: 0, y: 0, text: A + '|' + B + '|evals=' + globalThis.__module_evals, size: 1 });
};
