// 超时（v2）：`ctx.once(where, { timeoutMs })` + `try/catch`；timeout 是**失败截止**。
//
// 演示什么
//   · v1 = `afterScript(..., { timeoutMs })` + onTrigger 收 `ev.kind === "timeout"` + `ctx.finish(5)`；
//   · v2 = 一个 `ctx.once`：命中 ⇒ resolve 一个 `Tick`；到点 ⇒ **reject**，随后宿主自己 fail Run。
//   · ⛔ timeout 不是「等不到就跳过」：即使你 catch 了 rejection，Run 仍然会失败（父级裁决）。
//
// v2 新写法 / 门的边界
//   · 命中后写 guest 用 Tick：`tick.input.click(...)`（入队，下一读点落地）。
//   · catch 在**泵**里 drain 到（`in_callback_` 保持 false）⇒ 那里只能 log；
//     `ctx.fail` / `ctx.finish` / `ctx.quit` / `ctx.edit` / `ctx.input` 全是 TypeError。
//     timeout 已经由宿主记成 `once-timeout` 并 fail Run —— 不需要、也不能在 catch 里再 `ctx.fail`。
//     想换一条自己的失败原因：在 catch 里重新 `throw`（task 拒绝 ⇒ 宿主以这条 reason fail Run）。
//   · `ctx.fail` 的正确用武之地是 **Run 回调**（例如 `start` 里参数 / 订阅装失败）。
//
// 占位值
//   · script / label 坐标 / 按钮坐标 / timeoutMs 全是占位；`pos` 从 1 开始、`script` 含 .BIN。

import type { PluginModule, RunCtx } from "@amayui/lift-plugin";

const GATE_POINT = { script: "TITLE.BIN", pos: 0x00000000 } as const; // 占位
const GATE_TIMEOUT_MS = 30_000; // 占位：失败截止

const NEXT_X = 0; // 占位：门通过后的点击
const NEXT_Y = 0;

function start(ctx: RunCtx): void {
    ctx.task(async (t) => {
        // once 的门 = Run 回调 ⇒ 在 task 的同步首段注册（第一个 await 之前）。
        const gate = t.once(GATE_POINT, { timeoutMs: GATE_TIMEOUT_MS });

        try {
            const tick = await gate;
            tick.input.click(NEXT_X, NEXT_Y);
            log.info("info", "timeout: gate passed @" + String(tick.now.pos));
        } catch (err) {
            // 到点：宿主先 reject（本 catch 在同一泵里 drain 到）、随后 fail Run。
            // 这里只能 log；写类（含 ctx.fail）在泵里 TypeError。
            log.error("timeout: gate missed: " + String(err));
            // 自定义失败原因：重新抛出（task rejection ⇒ fail Run）。
            // 不抛也行 —— 宿主仍会以 once-timeout fail 这个 Run。
            throw err;
        }
    });

    log.info("info", "timeout: task registered; timeoutMs is a failure deadline");
}

const plugin = { start } satisfies PluginModule;

export default plugin;
