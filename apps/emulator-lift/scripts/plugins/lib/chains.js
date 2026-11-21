// scripts/plugins/lib/chains.js —— **通用**的链组合工具（与 save / 场景都无关）
//
// 链的契约（一个 chain 就是 `{ start, onTrigger }`，形状同 `PluginModule` 的那两个字段）：
//   · `start(ctx)`：发这条链的初始 `Trigger | Trigger[] | null`；
//   · `onTrigger(ctx, ev)`：只处理**属于本链**的事件，返回新的 armed 集合。
//   · ★ **无状态**：进度只在 `ev.name` 里（宿主每回调新建实例 —— `plugin-runtime.md` §4.6）；
//     ⛔ 不把进度塞进闭包 / `ctx.state`。
//   · ★ **两条链的 trigger 名字必须不相交**（本案：`save.js` 用 `load:` 前缀）。否则一个名字会被
//     另一条链抢走、**静默不触发**；`route` 不会替调用方查这件事（它只看 `isFirst(ev)`）。
//
// `route(isFirst, first, then)`：
//   · `start` **只启 `first`** —— `then` 由调用方在自己的 `onLoaded` 里 `then.start(ctx)` 拉起
//     （"载入完成才轮到场景"这件事属于调用方的业务，工具不猜时机）；
//   · `onTrigger` = `isFirst(ev) ? first.onTrigger(ctx, ev) : then.onTrigger(ctx, ev)`；
//   · `isFirst(ev)` 必须是**纯函数**（只看 `ev`，⛔ 不依赖闭包状态）；**timeout 事件也按名字路由**
//     （`ev.kind === "timeout"` 的 `ev.name` 就是那条链自己的名字）⇒ `isFirst` 只看名字就够。
//
// 组合示例（样板见 `./scenario-save07-battle.js`）：
//   export default route((ev) => String(ev.name).startsWith("load:"),
//                        loadSave(7, { expect: ["FIELD.BIN"], onLoaded: (ctx) => scenario.start(ctx) }),
//                        scenario);

/**
 * 把两条 chain 按事件归属拼成一个插件对象。
 *
 * @param {(ev: object) => boolean} isFirst 这个事件属于 `first`（true）还是 `then`（false）；纯函数。
 * @param {{ start(ctx): object|object[]|null, onTrigger(ctx, ev): object|object[]|null }} first
 * @param {{ start(ctx): object|object[]|null, onTrigger(ctx, ev): object|object[]|null }} then
 * @returns {{ start(ctx): object|object[]|null, onTrigger(ctx, ev): object|object[]|null }}
 */
export function route(isFirst, first, then) {
    if (typeof isFirst !== "function") {
        throw new Error("route: isFirst 必须是函数 (ev) => boolean");
    }
    if (first === null || typeof first !== "object" || typeof first.onTrigger !== "function") {
        throw new Error("route: first 必须是 chain（{ start, onTrigger }）");
    }
    if (then === null || typeof then !== "object" || typeof then.onTrigger !== "function") {
        throw new Error("route: then 必须是 chain（{ start, onTrigger }）");
    }
    return {
        start(ctx) {
            // ★ 只启 first；`then.start` 属于调用方的 `onLoaded`（见文件头）。
            return typeof first.start === "function" ? first.start(ctx) : null;
        },
        onTrigger(ctx, ev) {
            return isFirst(ev) ? first.onTrigger(ctx, ev) : then.onTrigger(ctx, ev);
        },
    };
}

export default route;
