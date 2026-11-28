// v2 夹具 · **每个 host→JS 入口末尾 drain**（同一预算窗）+ drain 的硬上限
//
// 判据（红了意味着什么）：
//   · `mode=ok`（`plugin/drain-per-entry`）：
//     ① `start` 里排的 microtask 必须**在同一个入口内**跑完 ⇒ 第一个读点之后的 dump 写进
//        guest 的 `phase` 是 **2**（没有 drain ⇒ 1）；
//     ② tick handler 里排的 microtask 必须**在 `runInstrHits` 内**跑完 ⇒ C++ 在 `instrHookProbe`
//        返回后立刻看到那一行 log（没有 drain ⇒ 要等下一个入口）。
//   · `mode=spin`（`plugin/drain-budget`）：`const f = () => Promise.resolve().then(f); f();`
//     ⇒ 微任务里的无限链必须被**同一预算窗**打断（deadline / op_limit），Run fail、
//     `pump()` 能返回（guest 不被卡死）。
const OUT_PHASE = 0xb110;

let phase = 1;
let tickLogs = 0;

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "spin") {
            const f = () => Promise.resolve().then(f);
            f();
            return;
        }
        if (mode !== "ok") throw new Error("drain-entry: unknown mode: " + String(mode));
        // ① `call()` 的 drain：排一个 continuation，它必须在本入口跑完。
        Promise.resolve().then(() => {
            phase = 2;
        });
        // ② `runInstrHits` 的 drain：tick handler 里排的 continuation 必须在本入口跑完。
        ctx.at({ script: "FIELD.BIN", pos: 0x100 }, (tick) => {
            Promise.resolve().then(() => {
                tickLogs += 1;
                log.info("tick-drained#" + tickLogs);
            });
        });
        // ③ 读点上的 dump：写 phase（Run 侧入口；ctx.edit 可用）。
        ctx.task(async (t) => {
            await t.delay(0);
            ctx.edit.global.set("int", OUT_PHASE, phase);
        });
    },
};

export default plugin;
