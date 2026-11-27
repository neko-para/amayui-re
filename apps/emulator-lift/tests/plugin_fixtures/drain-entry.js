// S2a 夹具 · **每个 host→JS 入口末尾 drain**（同一预算窗）+ drain 的硬上限
//
// 判据（红了意味着什么）：
//   · `mode=ok`（`plugin/drain-per-entry`）：
//     ① `start` 里排的 microtask 必须**在同一个入口内**跑完 ⇒ 第二个读点（`onTrigger`）
//        写进 guest 的 `phase` 是 **2**（没有 drain ⇒ 1，continuation 落到下一个入口才跑）；
//     ② tick 回调里排的 microtask 必须**在 `runInstrHits` 内**跑完 ⇒ C++ 在 `instrHookProbe`
//        返回后立刻看到那一行 log（没有 drain ⇒ 要等下一个入口）。
//     ⇒ 两个方向任一漏写，`plugin/drain-per-entry` 都红。
//   · `mode=spin`（`plugin/drain-budget`）：`const f = () => Promise.resolve().then(f); f();`
//     ⇒ 微任务里的无限链必须被**同一预算窗**打断（deadline / op_limit），Run fail、
//     `pump()` 能返回（guest 不被卡死）。★ 本 mode **没有 render**：否则下一帧 render 的
//     drain 会把残余 job 再跑一次，判据就说不清是哪条入口烧的。
const OUT_PHASE = 0xb110;

let phase = 1;
let tickLogs = 0;

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "spin") {
            const f = () => Promise.resolve().then(f);
            f();
            return [];
        }
        if (mode !== "ok") throw new Error("drain-entry: unknown mode: " + String(mode));
        // ① `call()` 的 drain：排一个 continuation，它必须在本入口跑完。
        Promise.resolve().then(() => {
            phase = 2;
        });
        // ② `runInstrHits` 的 drain：tick 回调里排的 continuation 必须在本入口跑完。
        const handle = ctx.hook.at("FIELD.BIN", 0x100);
        if (handle === undefined) throw new Error("drain-entry: ctx.hook.at failed");
        handle.onTick((tick) => {
            Promise.resolve().then(() => {
                tickLogs += 1;
                tick.log("info", "tick-drained#" + tickLogs);
            });
        });
        return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "t" })];
    },
    onTrigger(ctx) {
        ctx.edit.global.set("int", OUT_PHASE, phase);
        // ★ 再 arm 一条（不 fire 的区间）：Run 必须**活着**，后续 tick 探针才走得到
        //   （`return []` 会立刻收口 ⇒ hook 被释放 ⇒ 第二个方向判不了）。
        return [ctx.trigger.afterPosition("SCENE1", 0x2000, 0x2001, { name: "t2" })];
    },
};

export default plugin;
