// 旧配方 → v2 插件：加载存档（`ctx.task` + 顺序 `await ctx.once` + `tick.input.click`）。
//
// 演示什么
//   · v1 = 具名 Trigger 链（每个 onTrigger case 返回下一个 Trigger）+ `ctx.input.click` + profile；
//   · v2 = 一个 `ctx.task`：从头到尾是一条 async 函数，等待点用 `ctx.once({script,pos})`，
//     点击用**命中的那个 Tick** 上的 `tick.input.click(...)`（入队，下一读点落地）。
//   · 收口 = task settle + 没有其它订阅 ⇒ Run 自然结束（readout `run=done`）。
//     要指定 exit code 才用 `ctx.finish`，它只能在 Run 回调里调（task continuation / 泵里是 TypeError）。
//
// v2 新写法
//   · `ctx.once` 的门 = Run 回调 ⇒ **四个 once 都在第一个 await 之前注册**（task 的同步首段）。
//     这也意味着所有 timeout 的 deadline 都从 `start` 起算：总时长要留够；
//     「每段从上一段命中起算」在 v2 里要用多个 Run（once 不能在 continuation 里再注册）。
//   · `timeoutMs` 是**失败截止**：到点 reject 并 fail Run，不是「等不到就跳过」。
//   · `ctx.profile.begin/end` 也是 Run 回调能力（`inCallback()` 门）；v2 的 task continuation 里调会
//     TypeError，所以本例子不演示 profile —— 要 profile 就把它放在仍然存在的 Run 回调里。
//
// 占位值
//   · 脚本名 / label 坐标 / 按钮坐标 / 槽位全是占位。真实 `script` 必须是 marker 文件名（含 .BIN），
//     `pos` 从 1 开始；`DIALOG_POINT` / `CONFIRM_POINT` 是 SBUNKI 里两个不同的 label 坐标，这里都写 0 占位。

import type { PluginModule, RunCtx } from "@amayui/lift-plugin";

// ---------------------------------------------------------------------------
// 占位：等待点（script + label 坐标）
// ---------------------------------------------------------------------------
const TITLE_POINT = { script: "TITLE.BIN", pos: 0x00000000 } as const;
const SAVE_POINT = { script: "SAVE.BIN", pos: 0x00000000 } as const;
const DIALOG_POINT = { script: "SBUNKI.BIN", pos: 0x00000000 } as const;
const CONFIRM_POINT = { script: "SBUNKI.BIN", pos: 0x00000000 } as const;

// ---------------------------------------------------------------------------
// 占位：按钮坐标 / 槽位行
// ---------------------------------------------------------------------------
const LOAD_MENU_X = 0; // 占位：TITLE 上的 LoadData 按钮
const LOAD_MENU_Y = 0;
const SLOT_X = 0; // 占位：存档列表列坐标
const SLOT_ROW_Y0 = 0; // 占位：第 0 行 y
const SLOT_ROW_H = 0; // 占位：行高
const DIALOG_NO_X = 0; // 占位：教程询问「否」
const DIALOG_NO_Y = 0;
const DIALOG_OK_X = 0; // 占位：确认按钮
const DIALOG_OK_Y = 0;

const GATE_TIMEOUT_MS = 30_000; // 占位：整条链的失败截止（从 start 起算）

function rowY(slot: number): number {
    return SLOT_ROW_Y0 + slot * SLOT_ROW_H;
}

function slotFrom(params: Readonly<Record<string, string>>): number {
    const raw = params["slot"];
    const slot = raw === undefined ? 0 : Number(raw);
    if (!Number.isInteger(slot) || slot < 0) return 0;
    return slot;
}

function start(ctx: RunCtx): void {
    const slot = slotFrom(ctx.params);
    ctx.log("info", "load-save: start slot=" + String(slot));

    ctx.task(async (t) => {
        // ★ 四个 once 要在第一个 await 之前注册（once 的门 = Run 回调，task 的同步首段）。
        const title = t.once(TITLE_POINT, { timeoutMs: GATE_TIMEOUT_MS });
        const save = t.once(SAVE_POINT, { timeoutMs: GATE_TIMEOUT_MS });
        const dialog = t.once(DIALOG_POINT, { timeoutMs: GATE_TIMEOUT_MS });
        const confirm = t.once(CONFIRM_POINT, { timeoutMs: GATE_TIMEOUT_MS });

        // 旧配方：await script TITLE → click LoadData
        const titleTick = await title;
        titleTick.input.click(LOAD_MENU_X, LOAD_MENU_Y); // 入队；下一读点落地
        t.log("info", "load-save: LoadData clicked @" + String(titleTick.now.pos));

        // 旧配方：await position SAVE → click 第 slot 槽
        const saveTick = await save;
        saveTick.input.click(SLOT_X, rowY(slot));

        // 旧配方：await position SBUNKI（第一次询问）→ click「否」
        const dialogTick = await dialog;
        dialogTick.input.click(DIALOG_NO_X, DIALOG_NO_Y);
        t.log("debug", "load-save: tutorial no @" + String(dialogTick.now.pos));

        // 旧配方：await position SBUNKI（确认框）→ click 确认
        const confirmTick = await confirm;
        confirmTick.input.click(DIALOG_OK_X, DIALOG_OK_Y);
        t.log("info", "load-save: confirm clicked @" + String(confirmTick.now.pos));

        // task 返回 ⇒ settle；没有 at 订阅 ⇒ Run 收口（不需要 finish）。
    });
}

const plugin = { start } satisfies PluginModule;

export default plugin;
