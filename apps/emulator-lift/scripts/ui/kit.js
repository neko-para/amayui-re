// @ts-check
// emulator-lift · UI 组件库（**发行件**；与 `hud.js` 同目录，运行时读，改完重跑、⛔ 不重编）
//
// ★ 用法（**module** 脚本 —— `import` 是 ES module 语法；classic 脚本没有 import）：
//
//     import { Button } from './kit.js';
//
//     // S2：写 HUD 的脚本（`export default { render(ctx) }`）传自己的适配器
//     //     （`hud.js` 的 `pluginHost(ctx)`；只需要 { rect, text, hover, active, click }）。
//     Button({ key: 'profile', rect: {...}, label: 'PROFILE' }, host);
//
//     // 内部 hud 回退的经典 loader：第二个参数省略 ⇒ 走全局 `ui` / `game`
//     globalThis.render = function render() {
//       Button({ key: 'profile', rect: {...}, label: 'PROFILE' });
//     };
//
// ★ 分工（需求单「已裁决」第 2 条）：**C++ 只做原语** —— `ui.rect({key})` 的 resolved rect 是
//   命中区，`game.hover|active` 是只读 key，`game.click` 是一次性对象。**外观 / 状态 / 主题**
//   全在这里。⛔ 本文件不做通用布局系统、不做窗口管理、不自己算命中（那会变成第二套引擎）。
// ★ 状态：`idle` / `hover` / `active`（active 优先于 hover，与按下时"指针一定也在上面"一致）
//   + 可选的 `disabled`。★ disabled 的按钮**不挂 key** ⇒ 宿主不给它发 click（不可命中）。
// ★ 几何：`rect` 的 x/y/w/h 由调用方给（参考像素）。文本只做**左对齐 + 垂直居中**这一条算术
//   （⛔ 不调 `measure`：那要求宿主字体；组件库的几何不能被字体可用性绑架）。
// ★ 颜色 / 字号都在 `DEFAULT_THEME`；`theme` 覆盖是**整表替换**（不做逐键合并 —— 合并会让
//   "漏了一个状态"变成静默继承，而不是加载期/渲染期的显式错）。

/** @typedef {{ x: number, y: number, w: number, h: number }} ButtonRect */
/** @typedef {{ bg: string, fg: string }} ButtonColors */
/**
 * @typedef {{
 *   font: number,
 *   bold: boolean,
 *   text_pad_x: number,
 *   idle: ButtonColors,
 *   hover: ButtonColors,
 *   active: ButtonColors,
 *   disabled: ButtonColors,
 * }} ButtonTheme
 */
/**
 * @typedef {{
 *   key?: string,
 *   rect: ButtonRect,
 *   label?: string,
 *   disabled?: boolean,
 *   theme?: ButtonTheme,
 *   on?: { click?: (ev: UiClick) => void, hover?: (hovered: boolean) => void },
 * }} ButtonSpec
 */

/** 三态 + disabled 的默认配色（`#RRGGBBAA`；与 `hud.js` 的面板同色系）。 */
export const DEFAULT_THEME = {
  font: 14,
  bold: true,
  text_pad_x: 6,
  idle: { bg: '#1F2A36E6', fg: '#EBFFEB' },
  hover: { bg: '#2C3E50E6', fg: '#FFFFFFFF' },
  active: { bg: '#0E639CE6', fg: '#FFFFFFFF' },
  disabled: { bg: '#202020B3', fg: '#808080FF' },
};

/**
 * 内部 hud 回退 loader 的默认适配器（只有它知道全局 `ui` / `game`）。
 * @returns {{ rect: (o: UiRectOptions) => void, text: (o: UiTextOptions) => void,
 *             hover?: string, active?: string, click?: UiClick }}
 */
function classicHost() {
  return {
    rect: (o) => ui.rect(o),
    text: (o) => ui.text(o),
    get hover() { return game.hover; },
    get active() { return game.active; },
    get click() { return game.click; },
  };
}

/**
 * 画一个按钮（每帧调一次；display list 每次 render 前由宿主清空 ⇒ 这里只追加）。
 * @param {ButtonSpec} spec
 * @param {{ rect: (o: any) => void, text: (o: any) => void, hover?: string, active?: string, click?: UiClick }} [host]
 *   S2：像素原语的来路。省略 = hud 回退的全局 `ui` / `game`（内部路径）。
 * @returns {'idle' | 'hover' | 'active' | 'disabled'}
 */
export function Button(spec, host) {
  if (spec === undefined || spec === null || typeof spec !== 'object') {
    throw new TypeError('Button: 需要一个 { key, rect, label } 对象');
  }
  const rect = spec.rect;
  if (rect === undefined || rect === null || typeof rect !== 'object') {
    throw new TypeError('Button: rect 必须是 { x, y, w, h }（参考像素）');
  }
  const h = host === undefined || host === null ? classicHost() : host;
  const theme = spec.theme === undefined ? DEFAULT_THEME : spec.theme;
  const key = spec.key;
  const disabled = spec.disabled === true;
  // ★ 只读 key：没有 hover / active（宿主每帧交来的快照）⇒ idle。disabled 不看它们。
  const hovered = !disabled && key !== undefined && key !== '' && h.hover === key;
  const held = !disabled && key !== undefined && key !== '' && h.active === key;
  const state = disabled ? 'disabled' : (held ? 'active' : (hovered ? 'hover' : 'idle'));
  const colors = theme[state];

  // ① 底板：**带 key** 才是命中区（disabled 故意不带 ⇒ 宿主不会给它发 click）。
  /** @type {UiRectOptions} */
  const plate = { x: rect.x, y: rect.y, w: rect.w, h: rect.h, fill: colors.bg };
  if (!disabled && key !== undefined && key !== '') plate.key = key;
  h.rect(plate);

  // ② 文本：左对齐 + 垂直居中（⛔ 不量字，所以没有宿主字体也能画）。
  const label = spec.label === undefined ? '' : String(spec.label);
  if (label !== '') {
    h.text({
      x: rect.x + theme.text_pad_x,
      y: rect.y + Math.max(0, Math.round((rect.h - theme.font) / 2)),
      text: label,
      size: theme.font,
      color: colors.fg,
      bold: theme.bold,
    });
  }

  // ③ 事件钩子：在 **render** 里跑（⛔ 不在 handleEvent 里）。`host.click` 是宿主交来的
  //    一次性对象（这一帧看得到，下一次就没有）；只有 key 相同才触发。
  const handlers = spec.on === undefined ? {} : spec.on;
  if (typeof handlers.hover === 'function') handlers.hover(hovered);
  const click = h.click;
  if (!disabled && click !== undefined && click.key === key && typeof handlers.click === 'function') {
    handlers.click(click);
  }
  return state;
}
