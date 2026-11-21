// scripts/plugins/lib/save.js —— `loadSave(slot)`：**纯 JS 的 callback 链构造器**（C2c）
//
// 合同：`.tmp/plugin-runtime/c2c-decisions.md`（D-C2C-1…6）/ `scripts/plugins/plugin.d.ts`
//       （`RunCtx` / `Trigger` / `FiredEvent` / `TimeoutEvent`）。
//
// 链（slot 5 实测；**终态 = 调用方给的 `expect`**，⛔ 本文件不内置场景清单）：
//   TITLE [0x0460,0x0520) → click(1070,480) → SAVE [0x1104,0x11C4)
//   → move(300,rowY) → 读点 → click(300,rowY) → SBUNKI [0x0B18,0x0C58)
//   → click(640,323) → [expect 里任一脚本 | SBUNKI 0x0AF8]：
//        expect 命中 ⇒ done（给了 `onLoaded` 就交给它续）
//        SBUNKI [0x0AF8,0x0B18) ⇒ click(690,323) → [expect 里任一脚本] ⇒ done
//   ★ 为什么确认点击等 [0x0B18,0x0C58)：同一张「要读取吗？」对话框，`save07-battle.plan` 头的
//     `--input-plan-trace` 读数 —— 轮询体 [0x09A4,0x09F8) 与读段 [0x0AF8,0x0B18) 里点会被吞
//     （press 只跨 2 个读点），**读段之后** [0x0B18,0x0C58) 点 3/3 被消费。
//     ⇒ 旧窗口 [0x09F8,0x0B18) 的落点多数落在 0x0AF8（读段首）⇒ 点击被吞、链停在那里
//     （正是 `REQ-01M7W2X81F1ASB488EJFQGMQSH` 的签名：`gate timeout (load:field-2)`）。
//   rowY = 实测表 {0:91,1:150,2:209,3:269,4:328,5:388,6:447,7:507}
//          （⛔ 不是公式：7 的读数来自 `scripts/recipes/save07-battle.plan` 的槽 007；8–9 无读数 ⇒ 掷错）。
//
// ★ 2026-11-21（A 阶段）—— **两个接口，各管一件事**：
//   · `options.expect: string[]`（**必填**）：载入完成后落在哪个脚本，由**调用方按预期给** ——
//     ADV = `SC*` / `SG*` / `SP*`（如 `SC0000.BIN`）、战斗 = `BTL.BIN`、地图 = `FIELD.BIN`（可能还有别的名字）。
//     照宿主读数拼写、**带 `.BIN`**。终态 = `expect.map((s) => afterScript(s, { name: "load:expect:" + s }))`。
//     缺 / 空数组 / 元素不是非空字符串 ⇒ **构造时 throw**（消息里给出三类别名，⛔ 不猜）。
//   · `options.onLoaded(ctx)`：到终态**先 log 再回调**；它的返回值就是本次 `onTrigger` 的返回值
//     （`Trigger | Trigger[] | null`）。⛔ 没给 `onLoaded` 时行为一字不变（`return []`）；
//     它抛错就让它穿到宿主（⛔ 不 try/catch、⛔ 不代它 fail）。
//   · ★「载入 → 场景」的**组合**不在本文件：那是使用方拿 `./chains.js` 的 `route(isFirst, first, then)`
//     自己拼（样板 `./scenario-save07-battle.js`）。本文件**不认识场景**。
//
// ★ 本文件有一条 **default 导出**，但它**只是参考入口**：方便
//   `--plugin <本文件> --plugin-param slot=5 --plugin-param expect=FIELD,SC0000` 直接把本文件当插件跑
//   （headless e2e / 手工 run；`expect` 不给时用默认值**并打一行 info**）。
//   库消费者请用具名 `export function loadSave(slot, { expect, … })`；⛔ 不要依赖 default 的形状。
//   ⛔ 本文件**故意没有任何 import**：宿主 QJS 的 import 根 = **入口文件所在目录**
//   （`src/ui/script_quickjs.cpp` 的 `moduleNormalize`），`tests/plugin_fixtures/*` 不能 import
//   本文件；自带的 default 让 pure CTest 与 e2e 都能直接拿本文件当入口，不需要复制文件。
//
// ★ 失败面（全部响亮，⛔ 不静默）：
//   ① slot 非整数 / 不在 0–7 ⇒ **构造时** throw（参考入口在 `start` 里构造 ⇒ Run fail + 点名）；
//   ①' `expect` 缺 / 空 / 元素不是非空字符串 ⇒ **构造时** throw（消息给出 ADV / 战斗 / 地图三类别名）；
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

// 半开区间（实测；与 `scripts/recipes/save05-move.plan` / `save07-battle.plan` 逐字同源）。
const TITLE_LO = 0x0460;
const TITLE_HI = 0x0520;
const SAVE_LO = 0x1104;
const SAVE_HI = 0x11c4;
// ★ 确认框「要读取吗？」**读段之后**的窗口：plan 的 trace 实测 click 在这里 3/3 被消费。
//   （轮询体 [0x09A4,0x09F8) 与读段 [0x0AF8,0x0B18) 里点会被吞 —— 见文件头。）
const SBUNKI_CLICK_LO = 0x0b18;
const SBUNKI_CLICK_HI = 0x0c58;
// 第二道确认（同一张对话框的回环）：它只是载入流程的 UI 状态，不是场景类型 ⇒ 保留。
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

// 行 y 的**实测表**（⛔ 不用 91 + 59.5*n 之类的公式猜；0–6 与 7 都有读数）。
const ROW_Y = { 0: 91, 1: 150, 2: 209, 3: 269, 4: 328, 5: 388, 6: 447, 7: 507 };

const DEFAULT_GATE_TIMEOUT_MS = 30000;

// ★ 载入链的具名状态一律 `load:` 前缀；`chains.js` 的 `route` 与场景命名禁区都只看这一个前缀。
//   终态的具名状态 = `load:expect:<脚本名>`（脚本名 = 调用方给的 `expect` 元素，含 `.BIN`）。
const LOAD_EXPECT_PREFIX = "load:expect:";

/**
 * 构造一条 `loadSave(slot, options)` 的 callback 链（纯函数：只读 slot / options，不读 ctx）。
 *
 * @param {number} slot 存档槽 0–7（实测行表）。
 * @param {{
 *   expect: string[],
 *   timeoutMs?: number,
 *   onLoaded?: (ctx: object) => object|object[]|null,
 * }} options
 *   · `expect`（**必填**）：载入完成后**可以算落点**的脚本名表（照宿主读数拼写、带 `.BIN`）——
 *     ADV = `SC*`/`SG*`/`SP*`、战斗 = `BTL.BIN`、地图 = `FIELD.BIN` …；由**调用方按预期给**。
 *   · `timeoutMs`：每道门的超时；缺省 30000 ms。
 *   · `onLoaded`：载入**到终态**之后要做的事（终态先 `ctx.log` 再回调）；它的返回值就是本次
 *     `onTrigger` 的返回值。⚠ 抛错**不**在这里捕获 —— 交给宿主按既有规则记 fail。
 * @returns {{
 *   start(ctx): object,
 *   onTrigger(ctx, ev): object|object[]|null,
 * }}
 */
export function loadSave(slot, options = {}) {
    const rowY = ROW_Y[slot];
    if (!Number.isInteger(slot) || rowY === undefined) {
        throw new Error(`slot ${slot} has no measured row (only integers 0-7; see SAVE.md)`);
    }
    const gateTimeoutMs =
        options !== null && options.timeoutMs !== undefined ? options.timeoutMs : DEFAULT_GATE_TIMEOUT_MS;
    const onLoaded = options !== null ? options.onLoaded : undefined;
    if (onLoaded !== undefined && typeof onLoaded !== "function") {
        throw new Error(`onLoaded must be a function (got ${typeof onLoaded})`);
    }
    // ★ 终态由**调用方**给（⛔ 本文件不内置场景清单）：ADV / 战斗 / 地图的脚本名都不一样，
    //   只有使用方知道这次载入"应该落在哪"。缺了就当场 throw（⛔ 不猜一个默认值）。
    const expect = options !== null ? options.expect : undefined;
    if (!Array.isArray(expect) || expect.length === 0) {
        throw new Error(
            "loadSave: options.expect 必填（string[]，非空）—— 载入完成后落在哪个脚本由调用方按预期给：" +
                "ADV = SC*/SG*/SP*（如 SC0000.BIN）、战斗 = BTL.BIN、地图 = FIELD.BIN；" +
                "照宿主读数拼写（带 .BIN）",
        );
    }
    for (let i = 0; i < expect.length; i += 1) {
        if (typeof expect[i] !== "string" || expect[i].length === 0) {
            throw new Error(
                `loadSave: options.expect[${i}] 必须是非空字符串（脚本名，照宿主读数拼写、带 .BIN）`,
            );
        }
    }

    // 终态 = 调用方给的**期望落点表**（⛔ 本文件不内置场景清单）。
    // ★ 具名门：一个 armed 集合内 name 唯一；每个回调返回**完整的新集合**（⛔ 不是增量）。
    //   第二道确认之后也用它（那里同样只认调用方的 `expect`；点 690,323 那一步在 onTrigger 里）。
    const expectTerminals = (ctx) =>
        expect.map((script) =>
            ctx.trigger.afterScript(script, {
                name: LOAD_EXPECT_PREFIX + script,
                timeoutMs: gateTimeoutMs,
            }),
        );

    // 点完 640,323 之后的终态集合：`expect` 里任一脚本 **或** 第二道确认（同一张对话框的回环）。
    const terminal = (ctx) => {
        const gates = expectTerminals(ctx);
        gates.push(
            ctx.trigger.afterPosition(SCRIPT_SBUNKI, SBUNKI_CONFIRM_LO, SBUNKI_CONFIRM_HI, {
                name: "load:confirm2",
                timeoutMs: gateTimeoutMs,
            }),
        );
        return gates;
    };

    // 第二道确认之后的终态：只剩 `expect` 里的脚本。
    const terminalAfterConfirm = (ctx) => expectTerminals(ctx);

    const reached = (ctx, script) => {
        ctx.log("info", `load-save: reached ${script} (slot ${slot})`);
        // ★ 终态**先 log 再回调**：`onLoaded` 的返回值 = 本次 onTrigger 的返回值。
        //   ⛔ 没给 `onLoaded` ⇒ 与 C2c 一字不变（`return []` = done）。⛔ 不 try/catch：
        //   它抛错要按宿主的既有规则记 fail（有 file:line 归因），吞掉就变成"静默 done"。
        if (onLoaded === undefined) return [];
        return onLoaded(ctx);
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
                    return ctx.trigger.afterPosition(SCRIPT_SBUNKI, SBUNKI_CLICK_LO, SBUNKI_CLICK_HI, {
                        name: "load:sbunki",
                        timeoutMs: gateTimeoutMs,
                    });
                case "load:sbunki":
                    ctx.log("info", "load-save: SBUNKI hit (read-segment passed)");
                    ctx.input.click(CONFIRM_X, CONFIRM_Y);
                    return terminal(ctx);
                case "load:confirm2":
                    ctx.log("info", "load-save: SBUNKI 0x0AF8 ⇒ second confirm");
                    ctx.input.click(CONFIRM2_X, CONFIRM2_Y);
                    return terminalAfterConfirm(ctx);
                default:
                    // ★ 终态：`load:expect:<脚本名>`（脚本名 = 调用方给的 `expect` 元素）。
                    if (ev.name.startsWith(LOAD_EXPECT_PREFIX)) {
                        return reached(ctx, ev.name.slice(LOAD_EXPECT_PREFIX.length));
                    }
                    ctx.fail(ev.name, `unknown state: ${ev.name}`);
                    return [];
            }
        },
    };
}

// ---------------------------------------------------------------------------
// 参考入口（**只是参考**；库消费者用上面的 `loadSave`）
//
// `--plugin <本文件> --plugin-param slot=5 [--plugin-param expect=FIELD,SC0000]`
// （`ctx.params` 只读）。规则：
//   · `slot`：非整数 / 越界 ⇒ `loadSave` 构造时 throw ⇒ 宿主记 `run=failed`（进程 exit 不受影响）。
//   · `expect`：逗号分隔的**脚本名**（裸名自动补 `.BIN`，带 `.BIN` 也接受）。
//     ★ 不给 ⇒ 退回 `FIELD,SC0000` **并打一行 info**（"用的是默认 expect，可用 --plugin-param expect= 覆盖"；
//      ⛔ 不静默 —— 真实落点由调用方按预期给，这里只是让它能直接当插件跑）。
// ---------------------------------------------------------------------------
function slotFromParams(params) {
    return Number(params["slot"]);
}

const DEFAULT_EXPECT = ["FIELD.BIN", "SC0000.BIN"];

// null = 参数没给（由调用方决定用默认值 + 打一行说明）；数组 = 参数给了（可能为空 ⇒ 让 loadSave 响亮 throw）。
function expectFromParams(params) {
    const raw = params["expect"];
    if (raw === undefined || raw === null || String(raw).trim() === "") return null;
    const parts = String(raw).split(",");
    const out = [];
    for (let i = 0; i < parts.length; i += 1) {
        const name = parts[i].trim();
        if (name.length === 0) continue;
        out.push(name.endsWith(".BIN") ? name : name + ".BIN");
    }
    return out;
}

function loadOptionsFromParams(params) {
    const expect = expectFromParams(params);
    return { expect: expect === null ? DEFAULT_EXPECT : expect };
}

const plugin = {
    start(ctx) {
        if (expectFromParams(ctx.params) === null) {
            ctx.log(
                "info",
                `load-save: expect 未给 ⇒ 用默认 ${DEFAULT_EXPECT.join(",")}` +
                    "（可用 --plugin-param expect=FIELD,SC0000 覆盖）",
            );
        }
        return loadSave(slotFromParams(ctx.params), loadOptionsFromParams(ctx.params)).start(ctx);
    },
    onTrigger(ctx, ev) {
        return loadSave(slotFromParams(ctx.params), loadOptionsFromParams(ctx.params)).onTrigger(ctx, ev);
    },
};

export default plugin;
