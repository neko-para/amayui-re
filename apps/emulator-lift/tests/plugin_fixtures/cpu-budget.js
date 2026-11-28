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
//   · `mode=js-band`：只烧 CPU、**不调任何宿主能力**，而且**字节码远少于 10000 条**（引擎每 10000
//     条才调一次中断回调）⇒ 不被打断、正常跑完 ⇒ 判据落在"跑完了但 CPU 超了"的 `js` 档
//     （`callbackBudgetReason("start", "", false)`；⛔ 不是 `deadline` / `(host)`）。负载 =
//     **原生内建**的大循环（`Array#join`），一次调用烧几 ms CPU 而字节码只有几十条。
const SIGNAL = 0xb1b5;  // JS 写 1：宿主线程据此挂起主线程
const FLAG = 0xb1b4;    // 宿主线程恢复后写 1：JS 据此退出

const plugin = {
    start(ctx) {
        if (ctx.params.mode === "stall") {
            ctx.profile.begin("stall");  // ★ P2b：RunCtx 没有 edit ⇒ 用宿主桥把 SIGNAL 交给测试线程（见 C++）
            while (ctx.read.global.get("int", FLAG) !== 1) {
                // 空转等宿主线程：它会在挂起 6 ms 之后写 FLAG。
                // ⚠ 若宿主线程不响应，这里会被**CPU deadline**（2 ms）打断 ⇒ 用例红，不是挂死。
            }
            return;
        }
        if (ctx.params.mode === "js-band") {
            // 只烧 CPU、**不调宿主**，且总字节码 ≪ 10000（中断回调不跑 ⇒ 不被打断）：
            // 负载是原生内建的大循环（一次 join 在本机 ≈5 ms CPU，字节码只有几条）。
            // ⚠ 量级取自本机实测（2026-11-25 win32：300001 元素一次 ≈10.8 ms ⇒ 这里 ≈10 ms）；
            //   它只要**明显**超过 2 ms 就够（判据是"跑完了但 CPU 超了"），⛔ 不按某个精确值断言。
            let s = "";
            for (let i = 0; i < 2; i = i + 1) {
                s = new Array(150001).join("ab");
            }
            // 走到这里 = 没被打断（正确：本模式就是要"跑完"）；写长度只为别让引擎把负载当死代码。
            log.info("js-band: len=" + String(s.length));  // 走到这里 = 没被打断（负载没被当死代码）
            return;
        }
        // busy：只烧 CPU（没有宿主调用 ⇒ 打断只能来自预算）。
        // 上界只是"时钟/中断全坏时也别挂死"的兜底：正常情况下 2 ms 就被打断。
        let x = 0;
        for (let i = 0; i < 50000000; i = i + 1) {
            x = x + i;
        }
        log.info("busy: x=" + String(x));  // 走到这里 = 预算没触发（用例会因此红）
    },
};

export default plugin;
