// C2a 夹具 · `plugin/fail-install`（F2）：**onTrigger 返回的集合让 runner `install` 失败**
//
// 判据（红了意味着什么）：`install` 的失败（Once 对象再 arm / 重复 name / 非法 kind）只写进
// `TriggerRunner` 的账 ⇒ 没有同步的话 readout 会说 `run=waiting`，生产上只能等 `--time-limit`
// 才发现"这个 Run 其实已经死了"。
// ★ 这里用"同一个 Once 对象再 arm"（合同 §4.2 431 要求 fail），`install` 会点名 `trigger exhausted: a`。
let once;

const plugin = {
    start(ctx) {
        once = ctx.trigger.atReadPoint("any", 1, { name: "a" });
        return once;
    },
    onTrigger() {
        return once; // 同一 Once 对象放回 ⇒ exhausted ⇒ fail（⛔ 不是静默重置预算）
    },
};

export default plugin;
