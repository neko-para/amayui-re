// C2a 夹具 · `plugin/initial-state` 的反面：**没有** `initialState` ⇒ `ctx.state` 必须是 `{}`
// （⛔ 不是 undefined、也不是 null —— 插件一上手就写 `ctx.state.x = …` 是最常见的用法）。
const plugin = {
    start(ctx) {
        if (typeof ctx.state !== "object" || ctx.state === null) {
            ctx.fail("state", "没有 initialState 时 ctx.state 必须是对象");
            return [];
        }
        let keys = 0;
        for (const key in ctx.state) {
            keys += 1;
            void key;
        }
        if (keys !== 0) {
            ctx.fail("state", `没有 initialState 时 ctx.state 必须是空对象（有 ${keys} 个键）`);
            return [];
        }
        ctx.state.anything = 1; // 可写
        return [];
    },
    // ⛔ 成对：start 存在就必须有 onTrigger（合同 §4.6）。
    onTrigger() {
        return [];
    },
};

export default plugin;
