// C2a 夹具 · `control-script/global-read-modify-write`
//
// 判据：同一个回调栈里 `read → edit → input` 的顺序成立（合同 §4.4 508）：
//   · `ctx.read.global.get` 读到**旧值**；
//   · `ctx.edit.global.set` **立即**生效 ⇒ 紧接着的 `read` 读回**新值**（⛔ 不等任何读点）；
//   · Edit **不占** Input 的写预算 ⇒ `input.click` 另计（宿主侧用例断言队列深度）。
const plugin = {
    start(ctx) {
        return ctx.trigger.afterScript("SCENE1", { name: "rw" });
    },
    onTrigger(ctx, ev) {
        if (ev.name !== "rw") {
            ctx.fail(ev.name, "unexpected");
            return [];
        }
        const before = ctx.read.global.get("int", 0xb1b6);
        const receipt = ctx.edit.global.set("int", 0xb1b6, before | 0x10);
        const after = ctx.read.global.get("int", 0xb1b6);
        ctx.state.before = before;
        ctx.state.after = after;
        ctx.state.accepted = receipt.accepted && receipt.applied;
        const click = ctx.input.click(100, 200);
        ctx.state.clickId = click.id;
        return [];
    },
};

export default plugin;
