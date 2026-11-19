// C2a 夹具 · `start` 里的 `ctx.now` 与 Input 的落地时机（父级裁决 D-C2-10）
//
// 判据（两侧都由这个文件钉住）：
//   ① `start` 里 `ctx.now` **有值**（`script` / `pos` / `readIndex`）—— 宿主在 `begin` 之前补了
//      一次 `sampleNow`（否则 `atPoint` 在无 armed 时早退、根本没采样）；
//   ② `start` 里的 `ctx.input.click` **不在本读点落地**（落 `staged_`，**下一读点**才提升）。
// 期望值通过 `ctx.params` 传进来（⛔ 不在夹具里写死读数）。
const plugin = {
    start(ctx) {
        const expectScript = ctx.params.expect_script;
        const expectPos = Number(ctx.params.expect_pos);
        if (typeof ctx.now !== "object" || ctx.now === null) {
            ctx.fail("now", "start 里 ctx.now 是 undefined");
            return [];
        }
        if (ctx.now.script !== expectScript) {
            ctx.fail("now", `start.ctx.now.script=${String(ctx.now.script)}（期望 ${expectScript}）`);
            return [];
        }
        if (ctx.now.pos !== expectPos) {
            ctx.fail("now", `start.ctx.now.pos=${String(ctx.now.pos)}（期望 ${expectPos}）`);
            return [];
        }
        if (typeof ctx.now.readIndex !== "number" || ctx.now.readIndex <= 0) {
            ctx.fail("now", `start.ctx.now.readIndex=${String(ctx.now.readIndex)}`);
            return [];
        }
        ctx.input.click(11, 22);
        return ctx.trigger.atReadPoint("any", 1, { name: "next" });
    },
    onTrigger(ctx, ev) {
        if (ev.name !== "next") {
            ctx.fail(ev.name, "unexpected");
            return [];
        }
        return [];
    },
};

export default plugin;
