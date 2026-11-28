// v2 夹具 · `plugin-headless`（e2e）：一条**最小**的 Run 链，只证明"headless 也能装载 + 跑 start
// + 等一个真实脚本出现 + 正常收口"。
//
// 判据（红了意味着什么）：`--plugin` 在 `--present headless` 下装载/接线不通（读点观察者没接上、
// 脚本指令订阅没命中、或 Run 收不了口）。⛔ 不断言画面 / ⛔ 不断言 guest 业务正确性。
//
// ★ 为什么用 `once({script:"TITLE.BIN"})`：它只依赖"启动后一定会进 TITLE"（`--skip-logo` 之后），
//   不依赖"某条指令在 60 s 内出现"这种脆判据。60 s 超时 = 失败截止（`--time-limit 60` 之前
//   就会给出响亮原因，而不是静默等上限）。
const plugin = {
    start(ctx) {
        log.info("plugin-e2e start");
        ctx.task(async (t) => {
            await t.once({ script: "TITLE.BIN" }, { timeoutMs: 60000 });
            log.info("plugin-e2e fired");
        });
    },
};

export default plugin;
