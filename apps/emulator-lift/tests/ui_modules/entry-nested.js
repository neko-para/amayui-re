// 夹具：嵌套 import 的入口（②：A entry → B kit → C sub/nested）。
import { kitLabel } from './kit.js';

globalThis.render = function render() {
  ui.text({ x: 0, y: 0, text: kitLabel, size: 1 });
};
