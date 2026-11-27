// S0 夹具 · **Run 收口必须释放本 Run 的 hook**（缺陷 1）
//
// 判据（红了意味着什么）：`start` 里装的 `ctx.hook.at` + `patch` 在 Run 收口之后**还在**
//   ⇒ 后续脚本指令仍按条改写 guest（`instrHookList()` 非空）。三种收口各一条：
//   · `mode=fail`   ：`ctx.fail(...)`（S0 之前完全不清 hook）；
//   · `mode=done`   ：`return []`（install 把 runner 置 done；发生在回调**返回之后**）；
//   · `mode=budget` ：2 ms CPU 预算超限（`runCallback` 的失败分支；S0 之前不清 hook）。
//
// ★ 为什么先 `ctx.log` 一行：C++ 侧用它证明"hook 真的装上了" —— 否则"Run 结束后表为空"
//   在"at() 本来就失败"时也会绿（空夹具不能算通过，AGENTS.md §5）。
// ★ `script`/`pos` 只是形状（本用例不跑 AOT，探针永不命中）：`ctx.hook.at` 的判据是
//   "装上了"（句柄 + 表里一行），不是"真的命中过"。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;
// `cpu-budget.js` 的同一套握手槽（本夹具只借它做"走到这里 = 预算没触发"的落点）。
const BUDGET_MARK = 0xb1b6;

const plugin = {
    start(ctx) {
        const handle = ctx.hook.at(SCRIPT, POS);
        if (handle === undefined) throw new Error("ctx.hook.at failed（no_injection？）");
        const receipt = handle.patch({ ptrLocal: 0 }, 1);
        if (!receipt || receipt.applied === false) throw new Error("handle.patch failed");
        ctx.log("info", "hook-release: installed handle=" + handle.id);
        const mode = ctx.params.mode;
        if (mode === "fail") {
            ctx.fail("hook", "release-on-fail");
            return [];
        }
        if (mode === "budget") {
            // 只烧 CPU（没有宿主调用 ⇒ 打断只能来自预算）；走到下一行 = 预算没触发。
            let x = 0;
            for (let i = 0; i < 50000000; i = i + 1) {
                x = x + i;
            }
            ctx.edit.global.set("int", BUDGET_MARK, x);
            return [];
        }
        if (mode === "done") return [];
        throw new Error("hook-release: unknown mode: " + String(mode));
    },
    onTrigger(ctx) {
        void ctx;
        return [];
    },
};

export default plugin;
