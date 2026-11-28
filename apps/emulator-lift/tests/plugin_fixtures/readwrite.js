// v2 夹具 · `control-script/global-read-modify-write`
//
// 判据：同一个 Run 回调里 `read → edit → input` 的顺序成立：
//   · read 读到旧值；edit **立即**生效；edit 不占 Input 的写预算（队列里只有那一条 click）。
const plugin = {
    start(ctx) {
        const before = ctx.read.global.get("int", 0xb1b6);
        const receipt = ctx.edit.global.set("int", 0xb1b6, before | 0x10);
        const after = ctx.read.global.get("int", 0xb1b6);
        if (!(receipt.accepted && receipt.applied)) ctx.fail("readwrite", "edit not applied");
        if (after !== (before | 0x10)) ctx.fail("readwrite", "edit not immediate: " + String(after));
        ctx.input.click(100, 200);
    },
};

export default plugin;
