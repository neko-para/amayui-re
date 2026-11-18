// 夹具：单跳相对 import（①）。入口仍是 `globalThis.render`（module 里显式赋值，旧约定不变）。
import { greeting } from './text.js';

globalThis.render = function render() {
  ui.text({ x: 0, y: 0, text: greeting, size: 1 });
};
