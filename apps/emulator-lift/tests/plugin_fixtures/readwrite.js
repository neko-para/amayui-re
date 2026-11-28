// v2 夹具 · `control-script/global-read-modify-write`
//
// 判据：**同一个 tick 入口**里 `read → edit → input` 的顺序成立：
//   · read 读到旧值；edit **立即**生效；edit 不占 Input 的写预算（队列里只有那一条 click）。
//   ★ P2b：guest 写只走 `tick.edit`（RunCtx 没有 edit/input）⇒ 三条都在 handler 里做。
const PLACE = { script: "FIELD.BIN", pos: 0x100 };

const plugin = {
    start(ctx) {
        ctx.at(PLACE, (tick) => {
            const before = ctx.read.global.get("int", 0xb1b6);
            const receipt = tick.edit.global.set("int", 0xb1b6, before | 0x10);
            const after = ctx.read.global.get("int", 0xb1b6);
            if (!(receipt.accepted && receipt.applied)) ctx.fail("readwrite", "edit not applied");
            if (after !== (before | 0x10)) ctx.fail("readwrite", "edit not immediate: " + String(after));
            tick.input.click(100, 200);
            ctx.finish(0);
        });
    },
};

export default plugin;
