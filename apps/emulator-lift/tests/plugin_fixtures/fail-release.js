// v2 夹具 · `plugin/fail-releases-input`（F3 裁决的 fail 半）
//
// 判据：`ctx.fail(...)` 是**终态异常** ⇒ 同一个入口里**已经提交**的 input 必须被立刻丢掉。
// ★ P2b：guest 写只走 Tick ⇒ 在 at handler 里提交 click 再 fail（RunCtx 没有 input）。
const PLACE = { script: "FIELD.BIN", pos: 0x100 };

const plugin = {
    start(ctx) {
        ctx.at(PLACE, (tick) => {
            tick.input.click(200, 100);
            ctx.fail("boom", "stop-here");
        });
    },
};

export default plugin;
