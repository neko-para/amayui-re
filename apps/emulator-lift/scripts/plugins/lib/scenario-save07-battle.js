// scripts/plugins/lib/scenario-save07-battle.js —— **可跑样例**：载入存档 007 → 结束回合 → 決定 → 进战斗
//
// 这就是「新场景怎么写」的样板：**不重写载入存档那一段**（那是 `save.js` 的 `loadSave`），
// 只写"载入完成之后"要做的事，然后自己拿 `chains.js` 的 `route` 把两条链拼起来
// （`save.js` 不认识场景，也不提供"拼好的成品"）。
//
// 跑法（cwd = apps/emulator-lift；appdata 必须用 `scripts/clean-appdata.sh` 生成的那一份 ——
// 它复制 `saves/SAVE07.*` 等，页基 = 0 ⇒ 槽 007 才点得中，见 `scripts/recipes/save07-battle.plan` 头）：
//   out\emulator-lift-aot.exe --plugin scripts/plugins/lib/scenario-save07-battle.js ^
//     --game-dir ..\..\links\install --appdata <私有 appdata> ^
//     --skip-logo --audio off --present window --present-vsync 0 --time-limit 180 --frame-log
//
// ★ 为什么本文件必须在 `scripts/plugins/lib/` 里：宿主 QJS 的 import 根 = **入口文件所在目录**
//   （`src/ui/script_quickjs.cpp` 的 `moduleNormalize`），`../` 会被"越出 UI 根目录"拒掉、
//   子目录也解析不到 ⇒ 能 `import './save.js'` 的只有这一层。
// ★ **`expect` 由调用方按预期给**：这里 `['FIELD.BIN']` —— SAVE07 落在**地图**上，
//   战斗（`BTL.BIN`）是之后由用户动作（结束回合 / 決定）进的，不是载入落点。工具不猜。
// ★ 场景自己的 trigger 名⛔ 不许以 `load:` 开头（那是载入链的名字段；两条链名字必须不相交，
//   否则会被 `route` 的另一侧抢走、静默不触发）⇒ 本文件一律用 `battle:` 前缀。
// ★ 门 / 坐标与 `scripts/recipes/save07-battle.plan` 的 ④–⑧ 是**同一组读数**（⛔ 不在这里另立数值）：
//   FIELD 主轮询体 [0x579C,0x5860) → 结束回合 1220,45 → SELACT 主轮询体 [0x0834,0x08C0)
//   → 決定 640,517 → BTL 主循环 [0x4A84,0x4AE4)。
// ★ 全篇**零 `wait`**：每一道等都是 guest 可见状态门（零宿主墙钟、零时间等待）。
// ★ 无状态：进度只在 `ev.name`；宿主每回调新建实例也能接旧链（⛔ 不用 `ctx.state` 记步）。

import { route } from "./chains.js";
import { loadSave } from "./save.js";

// ★ marker 身份 = **文件名**（含 `.BIN`；C1 的门是精确 `==`，⛔ 不剥后缀）。
const SCRIPT_FIELD = "FIELD.BIN";
const SCRIPT_SELACT = "SELACT.BIN";
const SCRIPT_BTL = "BTL.BIN";

// 半开区间 [lo,hi)（与 `save07-battle.plan` 逐字同源）。
const FIELD_LO = 0x579c;
const FIELD_HI = 0x5860;
const SELACT_LO = 0x0834;
const SELACT_HI = 0x08c0;
const BTL_LO = 0x4a84;
const BTL_HI = 0x4ae4;

// 坐标（1280×720 参考系；与 plan 同源）。
const TURN_END_X = 1220; // 右上角「ターン終了」（结束回合）
const TURN_END_Y = 45;
const DECIDE_X = 640; // 中间「決定」
const DECIDE_Y = 517;

// 每道门的超时（与 plan 同档：FIELD 读档后要等地图就绪，SELACT / BTL 要等敌方回合与资源加载）。
const FIELD_TIMEOUT_MS = 300000;
const SELACT_TIMEOUT_MS = 180000;
const BTL_TIMEOUT_MS = 180000;

const scenario = {
    // 载入链到终态 ⇒ 本函数被调起（`onLoaded`）；返回的 trigger 就是新的 armed 集合。
    start(ctx) {
        ctx.log("info", "scenario: save07 loaded ⇒ waiting for FIELD main loop");
        return ctx.trigger.afterPosition(SCRIPT_FIELD, FIELD_LO, FIELD_HI, {
            name: "battle:field",
            timeoutMs: FIELD_TIMEOUT_MS,
        });
    },

    onTrigger(ctx, ev) {
        // 场景自己的门超时：响亮收口（⛔ 不静默 done；也就不会"看起来跑完了"）。
        if (ev.kind === "timeout") {
            ctx.finish(5, `scenario: gate timeout (${ev.name})`);
            return [];
        }
        switch (ev.name) {
            case "battle:field":
                ctx.log("info", "scenario: FIELD main loop hit ⇒ turn-end (1220,45)");
                ctx.input.click(TURN_END_X, TURN_END_Y);
                return ctx.trigger.afterPosition(SCRIPT_SELACT, SELACT_LO, SELACT_HI, {
                    name: "battle:selact",
                    timeoutMs: SELACT_TIMEOUT_MS,
                });
            case "battle:selact":
                ctx.log("info", "scenario: SELACT main loop hit ⇒ decide (640,517)");
                ctx.input.click(DECIDE_X, DECIDE_Y);
                return ctx.trigger.afterPosition(SCRIPT_BTL, BTL_LO, BTL_HI, {
                    name: "battle:btl",
                    timeoutMs: BTL_TIMEOUT_MS,
                });
            case "battle:btl":
                ctx.log("info", "scenario: BTL main loop hit");
                ctx.finish(0, "scenario: battle reached");
                return [];
            default:
                ctx.fail(ev.name, `unknown state: ${ev.name}`);
                return [];
        }
    },
};

// 槽 7 = 存档 007（行 y 507，见 `save.js` 的 ROW_Y 实测表）。
// 路由：`load:` 前缀（含 timeout 事件）归载入链，其余归场景 —— 这就是"使用方组合工具"。
export default route(
    (ev) => String(ev.name).startsWith("load:"),
    loadSave(7, {
        expect: ["FIELD.BIN"],  // ★ 载入落点：调用方按预期给（工具不猜）
        onLoaded: (ctx) => scenario.start(ctx),
    }),
    scenario,
);
