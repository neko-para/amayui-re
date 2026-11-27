// 重 arm（v2）：v1 的 Once / Reenter 两种 mode 与「把旧对象重新 arm」都不存在了。
//
// 演示什么
//   · v1 = Trigger 有 `mode`（once / reenter）和 `maxFires`；重新 arm = 把同一个对象再放进返回数组。
//   · v2 = `ctx.at` 订阅**默认每次命中都跑**；「只响一次」自己 `ctx.off`，「响 N 次」自己计数后 off；
//     「重新 arm」= 新的 `ctx.at(...)`（新订阅 / 新 id），不是复用旧对象。
//
// v2 新写法
//   · `ctx.at` 返回订阅 id；`ctx.off(id)` 幂等、tick handler 里也能调（只减）。
//   · 同一个点上可以有**多条**订阅，各自独立计数 / 各自 off（本文件装两条来对照 once / reenter 的等价物）。
//
// ⚠ v2 的门：`ctx.at` 只能在 **Run 回调**里调用（`start` / `onTrigger`；`ctx.task` 的同步首段也在
//   Run 回调里）。tick handler / task continuation 里再 `ctx.at` 会 TypeError —— 那里的写面只有
//   `tick.edit` / `tick.input` / `tick.patch`，加上只减的 `ctx.off`。
//   ⇒ 纯 v2 插件只有 `start` 一个 Run 回调：想在「一轮之后再来一轮」，
//     (a) 在 start 里把几轮要用的订阅都装好，用 `ctx.off` 选择谁在生效；或
//     (b) 结束当前 Run，由**下一个 Run 的 start** 重新装（跨 Run 的 re-arm）。
//
// 本文件演示 (a)：同一个点上装两条**新订阅**，各自独立计数 / 各自 `off`。
//
// 占位值
//   · script / label 坐标 / 次数全是占位；`pos` 真实值从 1 开始、`script` 含 .BIN。

import type { PluginModule, RunCtx } from "@amayui/lift-plugin";

const POINT = { script: "TITLE.BIN", pos: 0x00000000 } as const; // 占位

const ONCE_HITS = 1; // 占位：第一条订阅只响一次（v1 Once）
const REENTER_HITS = 3; // 占位：第二条订阅响三次后停（v1 Reenter + 自己数 maxFires）

let onceId: number | undefined;
let reenterId: number | undefined;
let onceHits = 0;
let reenterHits = 0;

function start(ctx: RunCtx): void {
    // 「重新 arm」= 新的 at 调用 ⇒ 新订阅（新 id）。两条订阅互相独立。
    onceId = ctx.at(POINT, (tick, next) => {
        onceHits += 1;
        tick.log("info", "rearm: once-sub hit " + String(onceHits));
        if (onceHits >= ONCE_HITS && onceId !== undefined) {
            ctx.off(onceId); // v1 Once 的等价物：命中即撤
        }
        next(); // 继续链（第二条订阅是另一个 id，仍会跑）
    });

    reenterId = ctx.at(POINT, (tick, next) => {
        reenterHits += 1;
        tick.log("info", "rearm: reenter-sub hit " + String(reenterHits));
        if (reenterHits >= REENTER_HITS && reenterId !== undefined) {
            ctx.off(reenterId); // 自己数到量再撤（v1 maxFires 的等价物）
        }
        next();
    });

    if (onceId === undefined || reenterId === undefined) {
        ctx.fail("rearm", "ctx.at 未装（no_injection）");
    }
    ctx.log("info", "rearm: two independent subscriptions installed (v2 has no mode)");
}

const plugin = { start } satisfies PluginModule;

export default plugin;
