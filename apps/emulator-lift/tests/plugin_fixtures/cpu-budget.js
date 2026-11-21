// C2a 夹具 · 2 ms 预算的**钟**：`busy` 真烧 CPU / `stall` 墙钟被拉长而 CPU 很小。
//
// 判据（红了意味着什么）：
//   · `mode=busy`：JS 体烧 >2 ms **CPU** ⇒ 回调必须以**预算**原因失败（reason 带
//     `interrupted` + `回调超出预算（deadline / op_limit）`），且 `overran()` 为真。
//     用例把 `op_limit` 设为 0（不限）⇒ 能打断它的**只可能**是 CPU deadline（⛔ 不是 op_limit 撞的）。
//   · `mode=stall`：JS 体先写 guest 全局 `SIGNAL` 告诉宿主线程"我已在跑"，再自旋等 `FLAG=1`；
//     宿主线程在这段等待里用 `SuspendThread` 把主线程挂起 6 ms ⇒ **墙钟 6 ms、CPU 只有几十 µs**。
//     （6 ms 故意 < 宿主安全网 20 ms：这条判的是"2 ms 判据用哪把钟"，⛔ 不是拿安全网兜。）
//     它**不许**因预算失败 —— 这正是"判据用 CPU 钟、不是墙钟"的对照（墙钟判据会误杀）。
const SIGNAL = 0xb1b5;  // JS 写 1：宿主线程据此挂起主线程
const FLAG = 0xb1b4;    // 宿主线程恢复后写 1：JS 据此退出

const plugin = {
    start(ctx) {
        if (ctx.params.mode === "stall") {
            ctx.edit.global.set("int", SIGNAL, 1);
            while (ctx.read.global.get("int", FLAG) !== 1) {
                // 空转等宿主线程：它会在挂起 6 ms 之后写 FLAG。
                // ⚠ 若宿主线程不响应，这里会被**CPU deadline**（2 ms）打断 ⇒ 用例红，不是挂死。
            }
            return [];
        }
        // busy：只烧 CPU（没有宿主调用 ⇒ 打断只能来自预算）。
        // 上界只是"时钟/中断全坏时也别挂死"的兜底：正常情况下 2 ms 就被打断。
        let x = 0;
        for (let i = 0; i < 50000000; i = i + 1) {
            x = x + i;
        }
        ctx.edit.global.set("int", SIGNAL, x);  // 走到这里 = 预算没触发（用例会因此红）
        return [];
    },
    onTrigger(ctx) {
        void ctx;
        return [];
    },
};

export default plugin;
