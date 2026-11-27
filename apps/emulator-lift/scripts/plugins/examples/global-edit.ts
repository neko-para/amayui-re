// 读改写 global（v2）：`ctx.at` 命中一条脚本指令，`tick.edit.global.set` **立即持久写**。
//
// 演示什么
//   · v1 = afterPosition Trigger + 在 onTrigger 里 `ctx.read.global.get` / `ctx.edit.global.set`；
//   · v2 = `ctx.at({script,pos}, handler)`：handler 在**指令点**跑，写面在 Tick 上。
//   · `tick.edit.global.set` 与 v1 的 `ctx.edit.global.set` 是**同一个宿主方法**：
//     立即持久写（不排队、不进 input 队列），下一读点就能读到新值。
//     ⛔ 它不是 `tick.patch` —— patch 是本拍瞬时覆盖、下一拍按 CAS 恢复，净变化 0。
//
// v2 新写法
//   · Tick 没有 read 面；要读旧值就在 start 里存下 RunCtx，用 `ctx.read.global.get`
//     （read 是只读口，tick 入口仍允许；写类 `ctx.edit` / `ctx.input` 在 tick 里是 TypeError）。
//   · 写只能走 `tick.edit` / `tick.input` / `tick.patch`。
//
// 占位值
//   · script / pos / global id / 掩码全是占位；`script` 真实写法必须是 marker 文件名（含 .BIN），
//     `pos` 从 1 开始（0 = 没给，运行期 TypeError）。

import type { PluginModule, RunCtx } from "@amayui/lift-plugin";

const SCRIPT = "SC0000.BIN"; // 占位：marker 文件名
const POS = 0x00000000; // 占位：label 坐标（真实值从 1 起）

const GLOBAL_ID = 0; // 占位：global 池下标
const SET_BITS = 0; // 占位：要置的位
const CLEAR_BITS = 0; // 占位：要清的位

/** start 存下来的 RunCtx：只用来读（`ctx.read` 不绑 in_callback，tick 入口仍可用）。 */
let runCtx: RunCtx | undefined;
let subId: number | undefined;

function start(ctx: RunCtx): void {
    runCtx = ctx;

    subId = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
        // 读旧值：tick 上没有 read；用存下来的 RunCtx（只读口）。
        const before = runCtx === undefined ? 0 : runCtx.read.global.get("int", GLOBAL_ID);
        const after = (before | SET_BITS) & ~CLEAR_BITS;

        // 写新值：立即持久写。回执的 applied 才代表写进去了。
        const receipt = tick.edit.global.set("int", GLOBAL_ID, after);
        if (!receipt.applied) {
            tick.log("warn", "global-edit: write rejected code=" + String(receipt.code));
        } else {
            tick.log(
                "debug",
                "global-edit: " + String(before) + " -> " + String(after),
            );
        }
    });

    if (subId === undefined) ctx.fail("global-edit", "ctx.at 未装（no_injection）");
}

const plugin = { start } satisfies PluginModule;

export default plugin;
