// v2 夹具 · `plugin/fail-releases-input`（F3 裁决的 fail 半）
//
// 判据：`ctx.fail(...)` 是**终态异常** ⇒ 同一个回调里**已经提交**的 input 必须被立刻丢掉。
const plugin = {
    start(ctx) {
        ctx.input.click(200, 100);
        ctx.fail("boom", "stop-here");
    },
};

export default plugin;
