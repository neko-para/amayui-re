// v2 夹具 · `plugin/quit-releases-input`（F3 裁决的 quit 半）
//
// 判据：`ctx.quit(...)` 也是**终态异常** ⇒ 同一次回调里已提交的 input 必须被立刻丢掉。
const plugin = {
    start(ctx) {
        ctx.input.click(300, 400);
        ctx.quit("scripted-quit");
    },
};

export default plugin;
