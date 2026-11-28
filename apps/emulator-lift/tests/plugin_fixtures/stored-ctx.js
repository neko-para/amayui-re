// C2b / S0 夹具 · `plugin/render-stored-runctx-rejected`：把 **RunCtx 存下来**在 render 里用
// `fail`/`finish`/`quit`/`profile.*`/**`hook.*`**/`at`/`delay` ⇒ 必须 **TypeError**。
//
// 判据（红了意味着什么）：render 期 `call_host` 非空（`ctx.read`/`ctx.log` 要用），而那些出口
//   只判"call_host 非空" ⇒ 插件在 `onTrigger`/`start` 里 `saved = ctx`，下一帧 `saved.fail(…)` /
//   `saved.hook.clear()` 就能绕过 `RenderCtx` 的成员表（`fail/finish/quit/profile/hook` 都不在它上面）。
//   本夹具把**被接受**的 op 收集起来自己抛（`accepted:<op,…>`）⇒ 宿主侧 render 返回 false ⇒ 红。
//   ★ 主机的 profile 桥是**接了的**（否则 `profile.*` 会因为"没接线"抛 TypeError 而假通过）；
//     C++ 侧同时断言 host_ops 的 begin/end 计数仍为 0。
//   ★ S0：`start` 里真装一条 hook（`savedHook`）—— C++ 侧拿 `instrHookList()` 做**基准**，
//     render 之后逐字段比对（表没变 ⇒ 那 7 个 hook 出口一个都没被接受）。
const OPS = [
    "fail",
    "finish",
    "quit",
    "profile.begin",
    "profile.end",
    // ★ S0（缺陷 4）：`ctx.hook` 是**唯一**漏了这一位门的写类出口（7 个 trampoline 都只判了
    //   `call_host != nullptr`）⇒ 存下来的 RunCtx 能在 render 里装 / 撤 / 覆盖正在跑的 Run 的 hook。
    "hook.at",
    "hook.list",
    "hook.clear",
    "hook.patch",
    "hook.capture",
    "hook.stats",
    "hook.handle.clear",
    // ★ S2a：`handle.onTick(fn)` 也是**只用于 Run 回调**的句柄出口（门 = `inCallback()`）；
    //   render 期（含**存下来的 RunCtx**）调它必须 TypeError —— 否则 render 里能注册 tick
    //   handler（静默越权：那会在下一次探针时跑用户 JS，且不经过任何入口的预算窗）。
    "hook.onTick",
    // ★ S2c：`ctx.at(where, fn)` 与 `once` / `task` 同一条门（`inCallback()`）⇒ render 里必须
    //   TypeError。**`off` 故意不进这张表**（它只减、门 = `call_host`；见下面的正向断言）。
    "at",
    // ★ P0：`ctx.delay(ms)` 的门 = Run 侧入口（render 里 `in_run_entry == false`）⇒ 同样 TypeError。
    "delay",
];

let saved = null;
let savedHook = null;

const plugin = {
    render(ctx) {
        // ★ 必须真的在 start 之后：`saved === null` 时 `saved.fail` 也会抛 TypeError（假通过）。
        if (saved === null || savedHook === null) throw new Error("render before start: RunCtx not stored");
        const accepted = [];
        for (const op of OPS) {
            try {
                if (op === "fail") saved.fail("x", "from-render");
                else if (op === "finish") saved.finish(0, "from-render");
                else if (op === "quit") saved.quit("from-render");
                else if (op === "profile.begin") saved.profile.begin("x");
                else if (op === "profile.end") saved.profile.end();
                else if (op === "hook.at") saved.hook.at("FIELD.BIN", 0x100);
                else if (op === "hook.list") saved.hook.list();
                else if (op === "hook.clear") saved.hook.clear();
                else if (op === "hook.patch") savedHook.patch({ ptrLocal: 0 }, 1);
                else if (op === "hook.capture") savedHook.capture({ ints: [0] });
                else if (op === "hook.stats") savedHook.stats();
                else if (op === "hook.handle.clear") savedHook.clear();
                else if (op === "hook.onTick") savedHook.onTick(() => {});
                else if (op === "at") saved.at({ script: "FIELD.BIN", pos: 0x100 }, () => {});
                else if (op === "delay") saved.delay(1);
                else throw new Error("unknown op: " + op);
            } catch (e) {
                // 唯一正确的形状：非回调期 ⇒ TypeError（⛔ 不是"接受"、也不是别的错）。
                if (!e || e.name !== "TypeError") {
                    throw new Error("wrong error kind for " + op + ": " + String(e));
                }
                continue;
            }
            accepted.push(op);
        }
        if (accepted.length !== 0) throw new Error("accepted:" + accepted.join(","));
        // ★ S2c：`ctx.off` 的门**故意与那 7 条不同**（只减操作，不写 guest、不让 Run 更长）⇒
        //   render 里调它必须被**接受**：不存在的句柄 ⇒ `false`（幂等），⛔ 不抛 TypeError。
        //   这是一条**正向**断言：把 `off` 塞进 OPS（或给它 `inCallback()` 门）会让它当场红。
        let offResult;
        try {
            offResult = saved.off(999999);
        } catch (e) {
            throw new Error("off rejected in render: " + String(e));
        }
        if (offResult !== false) throw new Error("off(999999) returned " + String(offResult));
        // ★ 同一次 render 里，读数面必须仍然可用（F2 的修法不能把 render 的 read/log 一起关掉）。
        if (typeof ctx.read.global.get !== "function") throw new Error("read lost");
        if (typeof ctx.log !== "function") throw new Error("log lost");
    },
    start(ctx) {
        saved = ctx;  // ★ 存下来 —— 这就是旁路的入口
        // ★ S0：同时存一条**真装上了**的 hook 句柄（render 里对它调 patch/capture/stats/clear）。
        const handle = ctx.hook.at("FIELD.BIN", 0x100);
        if (handle === undefined) throw new Error("start: ctx.hook.at failed");
        savedHook = handle;
        return [ctx.trigger.afterScript("TITLE", { name: "hold" })];
    },
    onTrigger() {
        return [];
    },
};

export default plugin;
