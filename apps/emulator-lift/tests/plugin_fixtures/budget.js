// C2b 夹具 · `plugin/render-budget-deadline`（复审 F3）：render 的预算被**绝对** deadline 打断。
//
// 判据（红了意味着什么）：`PluginScript::setBudget` 收到的是 `UiScript` 口径的**绝对**纳秒
//   （生产 = `present_sdl.cpp` 的 `steadyNowNs() + kUiScriptBudgetNs`）。若实现误加成相对
//   （`steadyNowNs() + deadline`），一个"已经过期"的 deadline 会被推到未来 ⇒ render 跑到底 ⇒ 红。
//
// ★ `for(;;)` 是承重的：中断回调每 ~10000 条字节码才跑一次；短脚本可能整段跑完都进不了中断。
const plugin = {
    render(ctx) {
        void ctx;
        for (;;) {}
    },
};

export default plugin;
