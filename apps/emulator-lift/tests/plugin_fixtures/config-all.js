// C2a 夹具 · `plugin/config-host-pause`：大域 `all()` 的**宿主 CPU** 不计进用户 2 ms
//
// 判据（红了意味着什么）：`ctx.config` 的值树 → JS 值的转换（纯宿主 C++：建字符串 / 定义属性）
// 被算成用户 JS 时间 ⇒ 一个"什么都没做、只是读配置"的回调会误报 `callback budget exceeded`。
// ★ 数据由 C++ 侧写进临时 appdata 的 `plugin-config.json`（域 `big`，1024 个长字符串 key）；
//   本夹具只做一次 `all()` 并核对大小（⛔ 不写 guest / 不落任何语义结论）。
const plugin = {
    start(ctx) {
        const all = ctx.config.domain("big").all();
        const keys = Object.keys(all);
        if (keys.length !== 1024) {
            ctx.fail("config-host-pause", `keys=${keys.length}`);
        }
        return [];
    },
    // ⛔ 成对：start 存在就必须有 onTrigger（合同 §4.6）。
    onTrigger() {
        return [];
    },
};

export default plugin;
