// 夹具：module 顶层抛错（⑥）—— 必须经 module 求值返回的 Promise 的 rejection 面被宿主看见，
// 而不是在 entry 里静默消失（entry 连 render 都没赋上）。
import { greeting } from './text.js';

throw new Error('module-boom');
