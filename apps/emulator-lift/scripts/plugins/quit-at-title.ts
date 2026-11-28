// scripts/plugins/quit-at-title.ts —— **TITLE → Quit** 的 e2e 驱动器（CTest `aot-quit-exit` 用；v2）
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
//   越出该根即拒）⇒ 要 import `lib/boot-to-title.ts`，入口必须与 `lib/` 共享同一个根。
//   `tests/plugin_fixtures/*` 往 `../../scripts/plugins/lib` 的 import 实测被拒
//   （"解析结果越出 UI 根目录"）⇒ 与 `scenario-*.ts` 同一个落点。
//
// ★ v2 形状（2026-11-28）：一条 `ctx.task` —— `await bootToTitle(t)` 拿到命中那一拍的 `Tick`，
//   在**同一个 continuation** 里 `tick.input.click(QUIT_X, QUIT_Y)`（Tick 的写面只在产生它的
//   那次入口内有效）。⛔ 不用 `ctx.input`（RunCtx 的写面在 task continuation 里是 TypeError）。
//   task 返回 ⇒ Run 自然收口（`run=done`），**不是** `finish` / `quit`。
// ★ 坐标是 1280×720 参考系（与旧 `.plan` 的 `click 1180 640` 同一点；`input::*` 的映射在
//   `cursor_map.h`，⛔ 本文件不做第二套换算）。
// ★ ⛔ 全篇零 `wait` / 零 `delay`：等的就是 guest 可见状态（门）。
import type { PluginModule, RunCtx } from "@amayui/lift-plugin";

import { bootToTitle } from "./lib/boot-to-title.ts";

/** TITLE 上 Quit 按钮的参考坐标（1280×720；与 `scripts/recipes/sn0000-quit.plan` 同一读数）。 */
const QUIT_X = 1180;
const QUIT_Y = 640;

const plugin = {
    start(ctx: RunCtx): void {
        ctx.task(async (t) => {
            const tick = await bootToTitle(t, { timeoutMs: 120000 });
            tick.input.click(QUIT_X, QUIT_Y);
        });
    },
} satisfies PluginModule;

export default plugin;
