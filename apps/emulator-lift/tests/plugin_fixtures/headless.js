// C2a 夹具 · `plugin-headless`（e2e）：一条**最小**的 Run 链，只证明"headless 也能装载 + 跑 start
// + 触发一个具名门 + 正常收口"。
//
// 判据（红了意味着什么）：`--plugin` 在 `--present headless` 下装载/接线不通（读点观察者没接上、
// `ctx.now` 没值、或 Run 收不了口）。⛔ 不断言画面 / ⛔ 不断言 guest 业务正确性。
//
// ★ 为什么用 `atReadPoint("any", 1)`：它只依赖"有读点"，与 boot 到哪一屏无关 ⇒ 不引入
//   "某脚本在 60 s 内一定出现"这种脆判据（那属于 C2c 的 load-save e2e）。
// ★ `ctx.log` 只打两行（一次性；宿主有界且去重）—— e2e 用它确认回调真的跑了。
const plugin = {
    start(ctx) {
        ctx.log("info", "plugin-e2e start");
        return ctx.trigger.atReadPoint("any", 1, { name: "e2e:first" });
    },
    onTrigger(ctx, ev) {
        if (ev.name !== "e2e:first") {
            ctx.fail(ev.name, `unknown name: ${ev.name}`);
            return [];
        }
        ctx.log("info", `plugin-e2e fired readIndex=${String(ctx.now.readIndex)}`);
        return [];
    },
};

export default plugin;
