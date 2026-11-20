// scripts/plugins/lib/save.js —— `loadSave(slot)`：**纯 JS 的 callback 链构造器**（C2c）
//
// 合同：`.tmp/plugin-runtime/c2c-decisions.md`（D-C2C-1…6）/ `scripts/plugins/plugin.d.ts`
//       （`RunCtx` / `Trigger` / `FiredEvent` / `TimeoutEvent`）。
//
// 链（slot 5 实测；三选一终端见下）：
//   TITLE [0x0460,0x0520) → click(1070,480) → SAVE [0x1104,0x11C4)
//   → move(300,rowY) → 读点 → click(300,rowY) → SBUNKI [0x09F8,0x0B18)
//   → click(640,323) → [FIELD | SC0000 | SBUNKI 0x0AF8]：
//        FIELD   ⇒ done（"点完 640,323 直接 FIELD"是**带 wait 的配方**读数；本链"进门即点"
//                  在 mac headless 实测先落进下面的 0x0AF8 ⇒ 第二道确认后才到 FIELD。
//                  三选一终端把两条路都接到 FIELD ⇒ 收口不依赖时序；见 c2c-report §5-2）
//        SC0000  ⇒ done（slot 4 形态）
//        SBUNKI [0x0AF8,0x0B18) ⇒ click(690,323) → [FIELD | SC0000] ⇒ done
//   rowY = 实测表 {0:91,1:150,2:209,3:269,4:328,5:388,6:447}（⛔ 不是公式；7–9 掷错 —— 只到 6 实测）。
//
// ★ 本文件有一条 **default 导出**，但它**只是参考入口**：方便
//   `--plugin <本文件> --plugin-param slot=5` 直接把本文件当插件跑（headless e2e / 手工 run）。
//   库消费者请用具名 `export function loadSave(slot)`；⛔ 不要依赖 default 的形状。
//   ⛔ 本文件**故意没有任何 import**：宿主 QJS 的 import 根 = **入口文件所在目录**
//   （`src/ui/script_quickjs.cpp` 的 `moduleNormalize`），`tests/plugin_fixtures/*` 不能 import
//   本文件；自带的 default 让 pure CTest 与 e2e 都能直接拿本文件当入口，不需要复制文件。
//
// ★ 失败面（全部响亮，⛔ 不静默）：
//   ① slot 非整数 / 不在 0–6 ⇒ **构造时** throw（参考入口在 `start` 里构造 ⇒ Run fail + 点名）；
//   ② 未知 `ev.name` ⇒ `ctx.fail(ev.name, …)` + `[]`；
//   ③ 门超时（`timeoutMs`）⇒ `ctx.finish(5, "load-save: gate timeout …")` + `[]`。
//      ⛔ 不用 `ctx.quit`（那会把归因改成 `plugin: quit`）/ ⛔ 不用 `ctx.run`（C5）。
// ★ 本对象**无状态**（进度只在 `ev.name`）：宿主每回调新建实例（`plugin-runtime.md` §4.6），
//   实现必须支持"新实例接旧链"；⛔ 不把进度塞进闭包 / `ctx.state`。

// ★ 脚本身份 = **marker 读数**（`PluginReadings::sample` ⇒ `MarkerSampler::resolveName`），实际是
//   文件名 `TITLE.BIN` / `SAVE.BIN` / …（实测：`--plugin` 探针在 headless boot 读到
//   `SYSTEM4.BIN` / `LOADCONFIG.BIN` / `TITLE.BIN`）。C1 的 `afterScript` / `afterPosition` 是
//   **精确 `==`**（`src/runtime/trigger_runner.cpp`：`now.script == trigger.script`），⛔ 不做
//   `await script` 那层的"两侧剥 `.BIN` + ASCII 大小写不敏感"（`src/runtime/recipe.cpp`）。
//   ⇒ 链里的名字必须带 `.BIN`，否则门永远不命中（纯假读数用例也要照这个拼写）。
//   ⚠ 这是 API 文档口径的已知缺口（`plugin.d.ts` / `plugin-runtime.md` 的样本写的是裸名
//   `TITLE`）：归 docs 轮裁决；本文件按**宿主实际读数**写，⛔ 不假设将来会归一化。
const SCRIPT_TITLE = "TITLE.BIN";
const SCRIPT_SAVE = "SAVE.BIN";
const SCRIPT_SBUNKI = "SBUNKI.BIN";
const SCRIPT_FIELD = "FIELD.BIN";
const SCRIPT_SC0000 = "SC0000.BIN";

// 半开区间（实测；与 `scripts/recipes/save05-move.plan` 逐字同源）。
const TITLE_LO = 0x0460;
const TITLE_HI = 0x0520;
const SAVE_LO = 0x1104;
const SAVE_HI = 0x11c4;
const SBUNKI_LO = 0x09f8;
const SBUNKI_HI = 0x0b18;
const SBUNKI_CONFIRM_LO = 0x0af8;
const SBUNKI_CONFIRM_HI = 0x0b18;

// 坐标（1280×720 参考系；实测）。
const LOAD_MENU_X = 1070;
const LOAD_MENU_Y = 480;
const SLOT_X = 300;
const CONFIRM_X = 640;
const CONFIRM_Y = 323;
const CONFIRM2_X = 690;
const CONFIRM2_Y = 323;

// 行 y 的**实测表**（⛔ 不用 91 + 59.5*n 之类的公式猜；只有 0–6 有读数）。
const ROW_Y = { 0: 91, 1: 150, 2: 209, 3: 269, 4: 328, 5: 388, 6: 447 };

const DEFAULT_GATE_TIMEOUT_MS = 30000;

/**
 * 构造一条 `loadSave(slot)` 的 callback 链（纯函数：只读 slot / options，不读 ctx）。
 *
 * @param {number} slot 存档槽 0–6（实测行表）。
 * @param {{ timeoutMs?: number }} [options] 每道门的超时；缺省 30000 ms。
 * @returns {{
 *   start(ctx): object,
 *   onTrigger(ctx, ev): object|object[]|null,
 * }}
 */
export function loadSave(slot, options = {}) {
    const rowY = ROW_Y[slot];
    if (!Number.isInteger(slot) || rowY === undefined) {
        throw new Error(`slot ${slot} has no measured row (only integers 0-6; see SAVE.md)`);
    }
    const gateTimeoutMs =
        options !== null && options.timeoutMs !== undefined ? options.timeoutMs : DEFAULT_GATE_TIMEOUT_MS;

    // 终态三选一（点完 640,323 之后的三种实测/推断形态）。
    // ★ 具名门：一个 armed 集合内 name 唯一；每个回调返回**完整的新集合**（⛔ 不是增量）。
    const terminal = (ctx) => [
        ctx.trigger.afterScript(SCRIPT_FIELD, { name: "load:field", timeoutMs: gateTimeoutMs }),
        ctx.trigger.afterScript(SCRIPT_SC0000, { name: "load:sc0000", timeoutMs: gateTimeoutMs }),
        ctx.trigger.afterPosition(SCRIPT_SBUNKI, SBUNKI_CONFIRM_LO, SBUNKI_CONFIRM_HI, {
            name: "load:confirm2",
            timeoutMs: gateTimeoutMs,
        }),
    ];

    // 第二道确认之后的终态：只剩 FIELD / SC0000。
    const terminalAfterConfirm = (ctx) => [
        ctx.trigger.afterScript(SCRIPT_FIELD, { name: "load:field-2", timeoutMs: gateTimeoutMs }),
        ctx.trigger.afterScript(SCRIPT_SC0000, { name: "load:sc0000-2", timeoutMs: gateTimeoutMs }),
    ];

    const reached = (ctx, script) => {
        ctx.log("info", `load-save: reached ${script} (slot ${slot})`);
        return [];
    };

    return {
        start(ctx) {
            ctx.log("info", `load-save: start (slot ${slot})`);
            return ctx.trigger.afterPosition(SCRIPT_TITLE, TITLE_LO, TITLE_HI, {
                name: "load:title",
                timeoutMs: gateTimeoutMs,
            });
        },

        onTrigger(ctx, ev) {
            if (ev.kind === "timeout") {
                // D-C2C-2：门超时 = finish(5)（⛔ 不用 quit：归因会变成 `plugin: quit`）。
                ctx.finish(5, `load-save: gate timeout (${ev.name})`);
                return [];
            }
            switch (ev.name) {
                case "load:title":
                    ctx.log("info", "load-save: TITLE hit");
                    ctx.input.click(LOAD_MENU_X, LOAD_MENU_Y);
                    return ctx.trigger.afterPosition(SCRIPT_SAVE, SAVE_LO, SAVE_HI, {
                        name: "load:save",
                        timeoutMs: gateTimeoutMs,
                    });
                case "load:save":
                    ctx.log("info", "load-save: SAVE hit");
                    ctx.input.move(SLOT_X, rowY);
                    return ctx.trigger.atReadPoint("any", 1, { name: "load:row", timeoutMs: gateTimeoutMs });
                case "load:row":
                    ctx.log("info", "load-save: row hovered");
                    ctx.input.click(SLOT_X, rowY);
                    return ctx.trigger.afterPosition(SCRIPT_SBUNKI, SBUNKI_LO, SBUNKI_HI, {
                        name: "load:sbunki",
                        timeoutMs: gateTimeoutMs,
                    });
                case "load:sbunki":
                    ctx.log("info", "load-save: SBUNKI hit");
                    ctx.input.click(CONFIRM_X, CONFIRM_Y);
                    return terminal(ctx);
                case "load:field":
                case "load:sc0000":
                    return reached(ctx, ev.name === "load:field" ? SCRIPT_FIELD : SCRIPT_SC0000);
                case "load:confirm2":
                    ctx.log("info", "load-save: SBUNKI 0x0AF8 ⇒ second confirm");
                    ctx.input.click(CONFIRM2_X, CONFIRM2_Y);
                    return terminalAfterConfirm(ctx);
                case "load:field-2":
                case "load:sc0000-2":
                    return reached(ctx, ev.name === "load:field-2" ? SCRIPT_FIELD : SCRIPT_SC0000);
                default:
                    ctx.fail(ev.name, `unknown state: ${ev.name}`);
                    return [];
            }
        },
    };
}

// ---------------------------------------------------------------------------
// 参考入口（**只是参考**；库消费者用上面的 `loadSave`）
//
// `start` 从 `ctx.params.slot` 构造、`onTrigger` 转发。参数只走 CLI 的
// `--plugin-param slot=5`（`ctx.params` 只读）；非整数 / 越界 ⇒ `loadSave` 构造时 throw
// ⇒ 宿主记 `run=failed reason=…`（进程 exit 不受影响）。
// ---------------------------------------------------------------------------
function slotFromParams(params) {
    return Number(params["slot"]);
}

const plugin = {
    start(ctx) {
        return loadSave(slotFromParams(ctx.params)).start(ctx);
    },
    onTrigger(ctx, ev) {
        return loadSave(slotFromParams(ctx.params)).onTrigger(ctx, ev);
    },
};

export default plugin;
