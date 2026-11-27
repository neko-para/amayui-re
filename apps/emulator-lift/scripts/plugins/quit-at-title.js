// scripts/plugins/quit-at-title.js —— **TITLE → Quit** 的 e2e 驱动器（CTest `aot-quit-exit` 用）
//
// 判据（红了意味着什么）：TITLE 的输入回调段门命中之后，点一次 **Quit**（1280×720 参考系的
//   `(1180,640)`），guest 自己的退出路径（`Command_Exit_Exception` 的 catch 续延 → `ExitProcess`）
//   必须走完 ⇒ 宿主进程 **exit 0**、`stop      : stopped`、派发表 `misses=0`，
//   并且**不是** `--time-limit` 打的圆场。
//   ⛔ 本文件**不许**用 `ctx.quit()` / `ctx.finish()` 收口：那测到的就是"插件叫停"，
//   不是"guest 自己的 Quit 路径"（判据会变成恒真）。
//
// 为什么在 `scripts/plugins/` 根（⛔ 不是 `tests/plugin_fixtures/`）：QJS 的 relative import 根 =
//   **入口文件所在目录**（`src/ui/script_quickjs.cpp` 的 `moduleNormalize` 的 `module_root`；
//   越出该根即拒）⇒ 要 import `lib/boot-to-title.js`，入口必须与 `lib/` 共享同一个根。
//   `tests/plugin_fixtures/*` 往 `../../scripts/plugins/lib` 的 import 实测被拒
//   （"解析结果越出 UI 根目录"）⇒ 与 `scenario-*.js` 同一个落点。
//
// ★ 链 = 唯一一道门（`bootToTitle`，`lib/boot-to-title.js`）：门命中 ⇒ `onDone` 点 Quit，返回 `[]`
//   （本链自己就是全部）。门超时 ⇒ `failOnTimeout` → `ctx.finish(5)`（⛔ 不用 `quit`：归因会变成
//   `plugin: quit`，判据就不是 guest 的退出路径了）。
// ★ 坐标是 1280×720 参考系（与旧 `.plan` 的 `click 1180 640` 同一点；`input::*` 的映射在
//   `cursor_map.h`，⛔ 本文件不做第二套换算）。
// ★ ⛔ 全篇零 `wait` / 零 `afterDelay`：等的就是 guest 可见状态（门）。
import { bootToTitle } from "./lib/boot-to-title.js";

/** TITLE 上 Quit 按钮的参考坐标（1280×720；与 `scripts/recipes/sn0000-quit.plan` 同一读数）。 */
const QUIT_X = 1180;
const QUIT_Y = 640;

export default bootToTitle({
    timeoutMs: 120000,
    onDone: (ctx) => {
        ctx.input.click(QUIT_X, QUIT_Y);
        return [];
    },
});
