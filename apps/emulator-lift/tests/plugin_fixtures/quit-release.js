// v2 夹具 · `plugin/quit-releases-input`（F3 裁决的 quit 半）
//
// 判据：`ctx.quit(...)` 也是**终态异常** ⇒ 同一个入口里已提交的 input 必须被立刻丢掉。
// ★ P2b：guest 写只走 Tick ⇒ 在 at handler 里提交 click 再 quit（RunCtx 没有 input）。
const PLACE = { script: "FIELD.BIN", pos: 0x100 };

const plugin = {
    start(ctx) {
        ctx.at(PLACE, (tick) => {
            tick.input.click(300, 400);
            ctx.quit("scripted-quit");
        });
    },
};

export default plugin;
