// P1-fix S2 夹具 · `ctx.waitAny(arms, { timeoutMs })`（多臂等待 + wall-clock 失败截止）
//
// 判据（每个 mode 对应一条 C++ 用例；红了意味着什么）：
//   · `two-arms`   ：两条臂 `{script,pos:0x200}`（第 0 条） / `{script,pos:0x100}`（第 1 条）；
//                    派发 0x100 ⇒ **第 1 条胜出** ⇒ resolve 的 `index = 1`、`tick.pos = 0x100`；
//                    两条临时 hook **都**撤掉（赢家自己 + 输的那条都不留 pending）。
//                    红了 = `index` 张冠李戴（当成第一条臂）/ 输的那条没撤（Run 只能等超时）。
//   · `timeout`    ：两条都等不到 + `timeoutMs=1` ⇒ 泵先 reject（catch 能 log，**不 rethrow**）、
//                    随后 fail Run（reason 点名 `wait-any timeout`）；hook 表清空。
//                    红了 = 超时只 reject 不 fail（Run 静默挂到 `--time-limit`）/ 条目残留。
//   · `timeout-finish`：catch 里 `ctx.finish(5, "<v1 原文案>")` 后再 throw ⇒ Run 的终态**不许**
//                    被翻成 failed（readout 仍是 `run=finished exit=5`；`gates.js` 的真实形状）。
//   · `clear`      ：`ctx.hook.clear()`（v1 的 onTrigger 里调）撤掉整组臂 ⇒ 整组 **reject**
//                    （catch 能 log；不 fail Run）⇒ task settle ⇒ Run **收口**（readout `run=done`）。
//                    红了 = `on_released` 只摘账不 reject ⇒ task 永远 pending ⇒ `run=waiting` 到上限。
//
// ★ 本夹具**只写模块变量 / log**（tick / 泵的 continuation 写不了 guest）；guest 写回不需要：
//   C++ 侧读 `logLines()` / `lastLogLine()` / `instrHookList()` / `readout()` 就能判。
const SCRIPT = "FIELD.BIN";
const POS_HIT = 0x0100;   // `dispatchInstr(f, kInstrPos)` 的落点（= 第 1 条臂）
const POS_MISS = 0x0200;  // 第 0 条臂：永远不命中
const POS_MISS2 = 0x0300; // 超时档的第二条：也不命中
const CLEAR_LO = 0x1000;  // v1 触发（`ctx.hook.clear()` 的入口；与既有夹具同形）
const CLEAR_HI = 0x1001;

let lastIndex = -1;

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "two-arms") {
            // ★ 第 1 条臂（下标 1）才是命中点 ⇒ 判 `index` 不会被"第一条"骗过。
            const pending = ctx.waitAny(
                [
                    { script: SCRIPT, pos: POS_MISS },
                    { script: SCRIPT, pos: POS_HIT },
                ],
                { timeoutMs: 5000 },
            );
            ctx.task(async (tctx) => {
                const hit = await pending;
                lastIndex = hit.index;
                // ★ 这条 log 是"task 的 continuation 在**本入口** drain 里跑完"的证据；
                //   C++ 侧断言它的文本（`index=1`）与 `lastLogLine()`。
                tctx.log(
                    "info",
                    "wait-any: index=" + String(hit.index) + " pos=" + String(hit.tick.pos) +
                        " script=" + String(hit.tick.now.script),
                );
            });
            ctx.log("info", "wait-any: armed two-arms");
            return null;  // 不返回 v1 集合：Run 靠 waitAny（+ task）活着
        }
        if (mode === "timeout") {
            const pending = ctx.waitAny(
                [
                    { script: SCRIPT, pos: POS_MISS },
                    { script: SCRIPT, pos: POS_MISS2 },
                ],
                { timeoutMs: 1 },
            );
            ctx.task(async (tctx) => {
                try {
                    const hit = await pending;
                    tctx.log("info", "wait-any: unexpected resolve index=" + String(hit.index));
                } catch (e) {
                    // ★ **不 rethrow**：让 task 正常 settle，交给泵的 timeout 路径 fail Run
                    //   （父级裁决：用户 catch 了也照样 fail，reason 由宿主点名）。
                    tctx.log("info", "wait-any: caught " + String(e && e.message ? e.message : e));
                }
            });
            ctx.log("info", "wait-any: armed timeout");
            return null;
        }
        if (mode === "timeout-finish") {
            const pending = ctx.waitAny(
                [
                    { script: SCRIPT, pos: POS_MISS },
                    { script: SCRIPT, pos: POS_MISS2 },
                ],
                { timeoutMs: 1 },
            );
            ctx.task(async (tctx) => {
                try {
                    await pending;
                } catch (e) {
                    // ★ `gates.js` 的真实形状：沿用 v1 的失败文案 + `finish(5)`，然后 throw。
                    tctx.finish(5, "load-save: gate timeout (load:expect:BTL.BIN)");
                    throw e;
                }
            });
            ctx.log("info", "wait-any: armed timeout-finish");
            return null;
        }
        if (mode === "clear") {
            const pending = ctx.waitAny(
                [
                    { script: SCRIPT, pos: POS_MISS },
                    { script: SCRIPT, pos: POS_MISS2 },
                ],
                { timeoutMs: 5000 },
            );
            ctx.task(async (tctx) => {
                try {
                    const hit = await pending;
                    tctx.log("info", "wait-any: unexpected resolve index=" + String(hit.index));
                } catch (e) {
                    tctx.log("info", "wait-any: cleared " + String(e && e.message ? e.message : e));
                }
            });
            ctx.log("info", "wait-any: armed clear");
            // ★ v1 触发只用来拿一个 **Run 回调**（`ctx.hook.clear()` 的门是 `inCallback()`）。
            return [ctx.trigger.afterPosition("SCENE1", CLEAR_LO, CLEAR_HI, { name: "clear" })];
        }
        ctx.fail("wait-any", "unknown mode " + String(mode));
        return null;
    },
    onTrigger(ctx, ev) {
        if (ev.kind === "trigger" && ev.name === "clear") {
            // ★ 整组被外部撤掉 ⇒ `ctx.waitAny` 必须 reject（否则 task 永远 pending）
            ctx.hook.clear();
        }
        return null;
    },
};

export default plugin;
