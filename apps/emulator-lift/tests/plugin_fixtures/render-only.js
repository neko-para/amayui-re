// C2a 夹具 · 只有 `render` 的插件（UI-only）必须**装载成功**（D-C2-5：有 render 但 headless 时
// 只打一条一次性 stderr、⛔ 不 exit 2；`hasRun()` 为 false ⇒ 没有 Run）。
const plugin = {
    render(ctx) {
        void ctx;
    },
};

export default plugin;
