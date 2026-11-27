// 并发 watch（v2）：一个 Run 里用 `ctx.task` 同时等多个 `ctx.once`。
//
// 演示什么
//   · v1 是「一次 arm 多个具名 Trigger + onTrigger 按 ev.name 分派」；v2 没有 armed 集合，
//     并发 = 在同一个 `ctx.task` 里持有多个 `ctx.once` 的 Promise，用 `Promise.race` / `Promise.all` 组合。
//   · 不新开 Run（v2 不允许嵌套 Run）：这些等待共享同一个 Run、同一个 RunCtx。
//
// v2 新写法
//   · `ctx.task(async (t) => …)`：`start` 只做注册；async 逻辑写在 task 里，`t` 就是当前 RunCtx。
//   · `ctx.once({script,pos}, {timeoutMs})`：返回 `Promise<Tick>`；命中后 resolve 一个 Tick，
//     写面在 Tick 上（`tick.input` / `tick.edit`），不是 `ctx.input` / `ctx.edit`。
//   · 全局 `log`（插件上下文）：task 的 continuation 里可以直接用。
//
// 占位值
//   · `script` 必须是 **marker 文件名（含 .BIN）**，宿主精确相等；`pos` 从 1 开始；
//     本文件的 script / pos / timeoutMs 全是占位，⛔ 不是可运行配方。
//
// ⚠ `once` 的 `timeoutMs` 是**失败截止**：到点不是「放弃等待」，而是宿主先 reject、随后 fail Run。
//   ⇒ `Promise.race` 只适合「两个点都必然会出现、只想先知道顺序」的场合；
//     真要取消一条等待只能结束 Run（once 没有取消 API）。

import type { PluginModule, RunCtx } from "@amayui/lift-plugin";

const TITLE_POINT = { script: "TITLE.BIN", pos: 0x00000000 } as const; // 占位
const FIELD_POINT = { script: "FIELD.BIN", pos: 0x00000000 } as const; // 占位

const GATE_TIMEOUT_MS = 30_000; // 占位：每个 once 自己的失败截止

function start(ctx: RunCtx): void {
    ctx.task(async (t) => {
        // ★ 两个 once 都必须在第一个 await 之前注册：once 的门 = Run 回调（task 的同步首段）。
        //   在 continuation 里再 `ctx.once(...)` 会 TypeError（那时已经不在 Run 回调里）。
        const title = t.once(TITLE_POINT, { timeoutMs: GATE_TIMEOUT_MS });
        const field = t.once(FIELD_POINT, { timeoutMs: GATE_TIMEOUT_MS });

        // 先到者：用它决定「先看到哪个屏」。
        const first = await Promise.race([title, field]);
        log.info("concurrent: first screen = " + first.now.script + " @" + String(first.now.pos));

        // 两个都到：race 只是先到者；Promise 本身还能继续 await（不会被 race 消费掉）。
        const [titleTick, fieldTick] = await Promise.all([title, field]);
        log.info(
            "concurrent: both seen = " +
                titleTick.now.script + " / " + fieldTick.now.script,
        );

        // task 返回（settle）且没有其它订阅 ⇒ Run 自然收口（不需要 finish）。
    });
    ctx.log("info", "concurrent: task registered");
}

const plugin = { start } satisfies PluginModule;

export default plugin;
