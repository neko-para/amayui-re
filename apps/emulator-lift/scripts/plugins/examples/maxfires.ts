// 次数上限（v2）：`ctx.at` + 计数 + 到量 `ctx.off`。
//
// 演示什么
//   · v1 = `afterPosition(..., { maxFires })`：预算属于 Trigger 对象、跨 arm 累计；
//     耗尽后再返回同一对象 = 宿主 fail。
//   · v2 没有 maxFires：订阅默认**每次命中都跑**，预算由插件自己数，到量就 `ctx.off(id)` 撤掉。
//     撤掉之后这条订阅不再是 Run 的存活来源（没有别的订阅 / task 时 Run 会收口）。
//
// v2 新写法
//   · `ctx.at` 的 handler 在**指令点**跑，`ctx.off` 在那里是允许的（门 = 插件上下文，不是 Run 回调）。
//   · `tick.log` 是这一拍可用的日志出口（与 `ctx.log` / 全局 `log` 是同一份宿主实现）。
//   · 计数放模块变量（per-plugin 状态），v2 不依赖 ctx.state。
//
// 占位值
//   · script / 区间 / MAX_FIRES 全是占位；真实 `lo < hi`、`script` 含 .BIN。

import type { PluginModule, RunCtx } from "@amayui/lift-plugin";

const SCRIPT = "SC0000.BIN"; // 占位
const POS_LO = 0x00000000; // 占位：[lo,hi)
const POS_HI = 0x00000000;

const MAX_FIRES = 2; // 占位：本文件用 2 让「到量」容易观察

let fires = 0; // 模块变量 = per-plugin 状态
let subId: number | undefined;

function start(ctx: RunCtx): void {
    subId = ctx.at({ script: SCRIPT, lo: POS_LO, hi: POS_HI }, (tick, next) => {
        fires += 1;
        tick.log("info", "maxfires: hit " + String(fires) + "/" + String(MAX_FIRES));

        if (fires >= MAX_FIRES) {
            // 到量：撤订阅（v1 的 maxFires 耗尽）。tick handler 里 off 是允许的。
            if (subId !== undefined) ctx.off(subId);
            tick.log("info", "maxfires: budget exhausted; subscription removed");
        }

        next(); // 继续同点后续 at handler（没有就只是示范链语义）
    });

    if (subId === undefined) ctx.fail("maxfires", "ctx.at 未装（no_injection）");
}

const plugin = { start } satisfies PluginModule;

export default plugin;
