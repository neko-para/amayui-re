// 旧配方 → 插件脚本：加载存档（第五轮，API = plugin.d.ts v5.3.2）。
//
// 翻译源（真实文件）：
//   apps/emulator-lift/scripts/recipes/title-loaddata.plan
//     await script TITLE timeout 30000 / wait 5000 / click 1070 480 / wait 20000
//   apps/emulator-lift/scripts/recipes/save04-fault.plan
//     await position SAVE … / click 第 5 槽 / await position SBUNKI … / wait 300 / click …
//
// ⛔ 本文件所有数值都是占位（0 / 未知区间），不是可运行配方；只用来评估 plugin.d.ts。
// v5.3.2：从 "@amayui/lift-plugin" 导入；默认导出 PluginModule；initialState 提供首读值。

import type {
    FiredEvent,
    InputReceipt,
    PluginModule,
    RunCtx,
    TimeoutEvent,
    Trigger,
} from "@amayui/lift-plugin";

// ---------------------------------------------------------------------------
// 具名状态（纯 string；拼错/重复只在运行期暴露，default 里 ctx.fail）
// ---------------------------------------------------------------------------
const N = {
    title: "load:title",
    titleSettle: "load:title-settle",
    save: "load:save",
    slot: "load:slot",
    slotSettle: "load:slot-settle",
    dialog: "load:dialog",
    dialogSettle: "load:dialog-settle",
} as const;

// ---------------------------------------------------------------------------
// 占位：脚本名 / 位置区间 / 按钮坐标 / 槽位
// ---------------------------------------------------------------------------
const SCRIPT_TITLE = "TITLE";
const SCRIPT_SAVE = "SAVE";
const SCRIPT_SBUNKI = "SBUNKI";

const SAVE_INPUT_LO = 0x00000000; // 占位：加载屏停点
const SAVE_INPUT_HI = 0x00000000;
const DIALOG_LO = 0x00000000; // 占位：第一次询问停点
const DIALOG_HI = 0x00000000;
const CONFIRM_LO = 0x00000000; // 占位：确认框停点
const CONFIRM_HI = 0x00000000;

const LOAD_MENU_X = 0; // 占位：TITLE 上 LoadData 按钮
const LOAD_MENU_Y = 0;
const SLOT_X = 0; // 占位：存档列表列坐标
const SLOT_ROW_Y0 = 0; // 占位：第 0 行 y
const SLOT_ROW_H = 0; // 占位：行高
const DIALOG_NO_X = 0; // 占位：教程询问"否"
const DIALOG_NO_Y = 0;
const DIALOG_OK_X = 0; // 占位：确认按钮
const DIALOG_OK_Y = 0;

const GATE_TIMEOUT_MS = 30000; // 旧配方行尾 timeout
const TITLE_SETTLE_MS = 5000; // 旧 wait 5000
const DIALOG_SETTLE_MS = 300; // 旧 wait 300
const CONFIRM_SETTLE_MS = 2000; // 旧 wait 2000

// per-Run 状态：attempts 由 initialState 给 0，首读即有值。
interface LoadState {
    attempts: number;
    lastInputId?: number;
    lastInputWhat?: string;
}

function rowY(slot: number): number {
    return SLOT_ROW_Y0 + slot * SLOT_ROW_H;
}

function slotFrom(params: Readonly<Record<string, string>>): number {
    const raw = params["slot"];
    const slot = raw === undefined ? 0 : Number(raw);
    if (!Number.isInteger(slot) || slot < 0) return 0;
    return slot;
}

// ---------------------------------------------------------------------------
// Input 完成度：click 只保证 queued；用 id 在下一个触发点查 status。
// ---------------------------------------------------------------------------
function noteInput(ctx: RunCtx<LoadState>, r: InputReceipt, what: string): void {
    if (!r.accepted) {
        ctx.log("warn", `input rejected: ${what}`, { code: r.code });
        return;
    }
    ctx.state.lastInputId = r.id;
    ctx.state.lastInputWhat = what;
}

function checkLastInput(ctx: RunCtx<LoadState>): void {
    const id = ctx.state.lastInputId;
    if (id === undefined) return;
    const st = ctx.input.status(id);
    if (st !== "applied") {
        ctx.log("warn", `input ${st} (not yet applied)`, {
            what: ctx.state.lastInputWhat,
        });
    }
}

// ---------------------------------------------------------------------------
// 链：start 返回第一个具名 Trigger；之后每个 case 返回下一个。
// ---------------------------------------------------------------------------
function loadSlot(slot: number): {
    start(ctx: RunCtx<LoadState>): Trigger;
    onTrigger(ctx: RunCtx<LoadState>, ev: FiredEvent | TimeoutEvent): Trigger | null;
} {
    return {
        // 旧：await script TITLE timeout 30000
        start(ctx: RunCtx<LoadState>): Trigger {
            return ctx.trigger.afterScript(SCRIPT_TITLE, {
                name: N.title,
                timeoutMs: GATE_TIMEOUT_MS,
            });
        },

        onTrigger(ctx: RunCtx<LoadState>, ev: FiredEvent | TimeoutEvent): Trigger | null {
            if (ev.kind === "timeout") {
                // 旧语义：门超时 ⇒ exit 5 + 点名。
                ctx.profile.end();
                ctx.finish(5, `load-save: gate timeout (${ev.name})`);
                return null;
            }

            switch (ev.name) {
                // 旧：wait 5000
                case N.title:
                    return ctx.trigger.afterDelay(TITLE_SETTLE_MS, { name: N.titleSettle });

                // 旧：click 1070 480
                case N.titleSettle:
                    noteInput(ctx, ctx.input.click(LOAD_MENU_X, LOAD_MENU_Y), "LoadData");
                    return ctx.trigger.afterPosition(SCRIPT_SAVE, SAVE_INPUT_LO, SAVE_INPUT_HI, {
                        name: N.save,
                        timeoutMs: GATE_TIMEOUT_MS,
                    });

                // 旧：click 300 328（第 slot 槽）
                case N.save:
                    noteInput(ctx, ctx.input.click(SLOT_X, rowY(slot)), `slot ${slot}`);
                    return ctx.trigger.afterPosition(SCRIPT_SBUNKI, DIALOG_LO, DIALOG_HI, {
                        name: N.slot,
                        timeoutMs: GATE_TIMEOUT_MS,
                    });

                // 旧：wait 300
                case N.slot:
                    return ctx.trigger.afterDelay(DIALOG_SETTLE_MS, { name: N.slotSettle });

                // 旧：click 640 323（教程询问选"否"）
                case N.slotSettle:
                    noteInput(ctx, ctx.input.click(DIALOG_NO_X, DIALOG_NO_Y), "tutorial no");
                    return ctx.trigger.afterPosition(SCRIPT_SBUNKI, CONFIRM_LO, CONFIRM_HI, {
                        name: N.dialog,
                        timeoutMs: GATE_TIMEOUT_MS,
                    });

                // 旧：wait 2000
                case N.dialog:
                    return ctx.trigger.afterDelay(CONFIRM_SETTLE_MS, { name: N.dialogSettle });

                // 旧：click 690 323（确认）→ 旧 save04-fault 的终点是 quit
                case N.dialogSettle:
                    noteInput(ctx, ctx.input.click(DIALOG_OK_X, DIALOG_OK_Y), "confirm");
                    ctx.profile.end();
                    // 若只想"加载后继续玩"，把这一句换成 ctx.finish(0, "load-save: done")。
                    ctx.quit("load-save: recipe end");
                    return null;

                default:
                    // fail(name?, reason?)：unknown name 放 name，原因放 reason。
                    ctx.fail(ev.name, "unknown state");
                    return null;
            }
        },
    };
}

// ---------------------------------------------------------------------------
// 插件入口
// ---------------------------------------------------------------------------
function start(ctx: RunCtx<LoadState>): Trigger | null {
    const slot = slotFrom(ctx.params);
    ctx.profile.begin("load-save");
    ctx.log("info", "load-save: start", { slot });
    return loadSlot(slot).start(ctx);
}

function onTrigger(ctx: RunCtx<LoadState>, ev: FiredEvent | TimeoutEvent): Trigger | null {
    ctx.state.attempts += 1; // initialState 给了 0，首读不是 undefined
    checkLastInput(ctx);
    return loadSlot(slotFrom(ctx.params)).onTrigger(ctx, ev);
}

const plugin = {
    initialState: { attempts: 0 },
    start,
    onTrigger,
} satisfies PluginModule<LoadState>;

export default plugin;
