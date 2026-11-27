// C2a/S2b 夹具 · **`start` 单独存在**（没有 `onTrigger`）⇒ 合法
//
// ★ S2b 的 v2 兼容层第一处放宽：v2 插件用 `ctx.task` / `ctx.once` 驱动，`start` 只做注册、
//   `return null`，不需要 `onTrigger`。同步契约不变：`start` 仍必须同步返回
//   `Trigger | Trigger[] | null | undefined`（返回 Promise ⇒ 装载期明确报错）。
// ★ 本夹具不注册任何异步工作 ⇒ `return null` 之后 Run 立刻收口（`callbacks=1` / `run=done`）。
const plugin = {
    start(ctx) {
        ctx.log("info", "start-only: registered");
        return null;
    },
};

export default plugin;
