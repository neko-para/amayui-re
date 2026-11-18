// 夹具：嵌套链的中间模块（B）—— 自己再 import 一个子目录里的模块（C）。
import { NESTED } from './sub/nested.js';

export const kitLabel = 'kit:' + NESTED;
