// scripts/plugins/lib/recipe.js —— `runRecipe(plan)`：把 `Step[]` 翻成 **callback 链**（C3）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §5 C3（`:1162-1206`）；母单
//       `data/requirements/01M7T0VJPG2S0RZ1HNWW0HH3HY.md:51`（"JS recipe driver + parity（`InputPlan` 保留）"）。
// 判据：CTest `recipe-driver-parity`（`tests/recipe_driver_parity.cpp`）——oracle = 旧执行器
//       `InputPlan`（**生产真源**，⛔ 不切换），driver = 本文件。
//
// ★ **只做 parity，不接生产**：`--input-plan` 仍走 `InputPlan`；本文件只由 parity 夹具加载
//   （合同 `:1186-1188`）。
// ★ **不执行 C++ 的那份判据**：门的成立 / 超时 / 完成度**仍然只有一份**（`runtime/recipe.h`）。
//   本文件只做**翻译**：九条指令（`StepKind` 闭集合）+ 行尾 `timeout` + `quit` + `profile_*`。
//   ⛔ 不重新实现 `stepGateHolds` / `stepTimeoutMs` / `stepWindowExpired`（那是两份判据 ⇒ 一定静默漂）。
//
// ---- 输入形状（**唯一**来路 = `src/runtime/recipe_json.cpp` 的 `stepsJson()`）----
//
//     { origin: "<配方路径>",
//       steps: [ { id, kind, line, script, posFrom, posTo, waitMs, x, y, button, label, timeoutMs } ] }
//
//   ★ 为什么是"JS 模块"而不是"ctx.params 里放一个 JSON 文件路径"：宿主的 QJS 沙箱**没有**文件 API /
//     `JSON`（`src/ui/script_quickjs.cpp:9-22` 的 `JS_NewContextRaw` + 只加 BaseObjects；`:1965`），
//     而 import 只收相对路径且必须落在入口目录之下（`:1234-1250`）⇒ 模块装载是唯一"不新增宿主 API
//     又能读宿主文件"的通道。parity 夹具把本文件**逐字节复制**进 run 私有目录，并把 `stepsJson()`
//     的结果写成 `recipe-steps.js`（`export default <那一个 JSON 对象>`）。⛔ 不给 `ctx` 加成员
//     （`plugin/api-surface` 是那条的机械判据）。
//
// ---- 链（每个回调**消费一步**；非阻塞的步骤在同一个回调里连续消费）----
//
//   `await script`   → `ctx.trigger.afterScript(script, { name:"recipe:<i>", timeoutMs? })`
//   `await position` → `ctx.trigger.afterPosition(script, posFrom, posTo, { name:"recipe:<i>", timeoutMs? })`
//   `wait`           → `ctx.trigger.afterDelay(waitMs, { name:"recipe:<i>" })`（宿主墙钟；NON-DIFFABLE）
//   `click` / `mouse_move` / `mouse_down` / `mouse_up` → `ctx.input.*`（**下一读点**落地）
//   `profile_begin` / `profile_end` → `ctx.profile.begin(label)` / `ctx.profile.end()`（HostEffect）
//   `quit`           → `ctx.finish(剩余 ? 5 : 0, …)` + `ctx.quit(…)`（旧执行器：`quit` 之后还有指令 ⇒
//                       `outcome()==Running` ⇒ exit 5；`quit` 是最后一条 ⇒ `Done` ⇒ 0）
//   全部消费完       → `ctx.finish(0, "recipe: done applied=N pending=0 …")`（= 正常收口，⛔ 不是 quit）
//
//   ★ **自己没有进度状态机以外的语义**：`applied` = 已消费的指令数，`pending` = `steps.length − applied`；
//     它们**只**出现在收口原因 / 失败原因里（读数），⛔ 不参与门判定。
//   ★ `ctx.state` **不用**：链的游标在闭包里（与 `lib/save.js` 的"无状态链"同一条口径——宿主每个
//     回调新建 `RunCtx`，但模块只求值一次 ⇒ 闭包状态跨回调存活）。⛔ 不把进度塞进 guest / `ctx.state`。
//   ★ 具名 = `recipe:<下标>`（0 基；同一个 armed 集合里唯一）。parity 夹具用它读 driver 的进度。
//
// ---- 已列出的缺口（⛔ 不假装覆盖；差异表在 `tests/recipe_driver_parity.cpp` 的 `kKnownDiffs`）----
//
//   ① `mouse_down` / `mouse_up`：C1 的 `IntentExecutor::drainQueue` **不施加**显式 down/up
//      （`src/runtime/intent_executor.cpp:128-133`，留 C4）⇒ 它们只能入队（队首头阻）⇒ 带拖拽的配方
//      NON-DIFFABLE（本文件**照旧提交**，⛔ 不静默丢掉——那会把"缺口"变成"driver 少了一条指令"）。
//   ② `timeout` 边界：宿主 trigger 是 `elapsed >= timeoutMs`，旧执行器 `stepWindowExpired` 是严格 `>`
//      （`src/runtime/recipe.cpp:249`）⇒ **1 ms 差异**（只在边界那一刻可观测；⛔ 不改共享判据）。
//   ③ `exit 5` 只能映射 `ctx.finish(5)` 的 receipt；旧执行器的 `summary/failureReport/fault/trace/exit5`
//      仍是生产真源（合同 `:1178`）。
//   ④ script 名字：`afterScript/afterPosition` 走宿主**精确相等**（`trigger_runner.cpp:67-87`），
//      而旧执行器 `scriptNameMatches` 还做 ASCII 大小写不敏感 + 两侧剥 `.BIN` + 接受 `id:0x%08X` 退化名
//      （`src/runtime/recipe.cpp:155-158`）⇒ 本文件**按原文**注册，不做归一化（两份归一化 = 必漂）。
//   ⑤ `profile_end <label>`：`ProfileApi.end()` 没有 label（`plugin.d.ts:272-275`）⇒ label 只用于 begin。
//
// ★ 失败面**全部响亮**（⛔ 不静默）：未知 kind / 回执 `accepted !== true` / 触发了不是当前步骤的
//   trigger / 门超时 ⇒ `ctx.fail` 或 `ctx.finish(5)` 并点名 `recipe:<下标>` + 行号。

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").TriggerOptions} TriggerOptions */
/** @typedef {import("@amayui/lift-plugin").InputReceipt} InputReceipt */
/** @typedef {import("@amayui/lift-plugin").MouseButton} MouseButton */
/**
 * 本文件看到的 `onTrigger` 事件（宿主只会交 `trigger` / `timeout` 两种；`name` 在两种上都必填，
 * 这里按 `?` 标，是因为本文件的失败面**防御性地**保留了一条 `name` 缺失时的 `recipe:?` 分支）。
 *
 * @typedef {import("@amayui/lift-plugin").TriggerEventBase
 *   & { readonly kind: "trigger" | "timeout", readonly name?: string }} RecipeEvent
 */
/**
 * 本文件产出的链：形状同 `PluginModule` 的 `start` / `onTrigger`（事件按上面的 `RecipeEvent`）。
 *
 * @typedef {object} RecipeChain
 * @property {(ctx: RunCtx) => Trigger | Trigger[] | null} start
 * @property {(ctx: RunCtx, ev: RecipeEvent) => Trigger | Trigger[] | null} onTrigger
 */
/**
 * `stepsJson()` 的一步：形状 = `src/runtime/recipe_json.cpp` 的 `stepJson`
 * （13 个键**总是全在**；`id` / `script` / `label` 是字符串，其余是数字，`button` 是词表）。
 *
 * @typedef {object} RecipeStep
 * @property {string} id
 * @property {string} kind
 * @property {number} line
 * @property {string} script
 * @property {number} posFrom
 * @property {number} posTo
 * @property {number} waitMs
 * @property {number} x
 * @property {number} y
 * @property {MouseButton} button
 * @property {string} label
 * @property {number} timeoutMs
 */

const KIND_AWAIT_SCRIPT = "await script";
const KIND_AWAIT_POSITION = "await position";
const KIND_WAIT = "wait";
const KIND_CLICK = "click";
const KIND_MOUSE_MOVE = "mouse_move";
const KIND_MOUSE_DOWN = "mouse_down";
const KIND_MOUSE_UP = "mouse_up";
const KIND_PROFILE_BEGIN = "profile_begin";
const KIND_PROFILE_END = "profile_end";
const KIND_QUIT = "quit";

// 旧执行器的"没走完"退出码（`recipe.h` 的 `kRecipeIncompleteExitCode`；⛔ 不在这里另定一个数）。
const INCOMPLETE_EXIT_CODE = 5;

/**
 * @param {number} v
 * @returns {string}
 */
function hex8(v) {
    let text = (v >>> 0).toString(16).toUpperCase();
    while (text.length < 8) text = "0" + text;
    return "0x" + text;
}

// 人可读的一步（失败 / 超时原因里点名它；⛔ 不参与判定）。
/**
 * @param {RecipeStep} step
 * @returns {string}
 */
function stepTextOf(step) {
    const line = "（行 " + step.line + "）";
    if (step.kind === KIND_AWAIT_SCRIPT) return "await script " + step.script + line;
    if (step.kind === KIND_AWAIT_POSITION) {
        return "await position " + step.script + " " + hex8(step.posFrom) + " " + hex8(step.posTo) + line;
    }
    if (step.kind === KIND_WAIT) return "wait " + step.waitMs + line;
    if (step.kind === KIND_CLICK) return "click " + step.x + " " + step.y + " " + step.button + line;
    if (step.kind === KIND_MOUSE_MOVE) return "mouse_move " + step.x + " " + step.y + line;
    if (step.kind === KIND_MOUSE_DOWN) return "mouse_down " + step.button + line;
    if (step.kind === KIND_MOUSE_UP) return "mouse_up " + step.button + line;
    if (step.kind === KIND_PROFILE_BEGIN) return "profile_begin " + step.label + line;
    if (step.kind === KIND_PROFILE_END) return "profile_end " + step.label + line;
    if (step.kind === KIND_QUIT) return "quit" + line;
    return "?" + line;
}

/**
 * @param {string} kind
 * @returns {boolean}
 */
function isGateKind(kind) {
    return kind === KIND_AWAIT_SCRIPT || kind === KIND_AWAIT_POSITION;
}

/**
 * @param {string} kind
 * @returns {boolean}
 */
function isWriteKind(kind) {
    return kind === KIND_CLICK || kind === KIND_MOUSE_MOVE || kind === KIND_MOUSE_DOWN || kind === KIND_MOUSE_UP;
}

// 行尾 `timeout <ms>`：0 = 这一行没写 ⇒ **不设**超时（⛔ 不下发 `timeoutMs: 0`：0 在宿主里也是
// "不设"，但显式省略能让"没写"与"写了 0"在读数上分得开——配方解析期本来就不收 0）。
/**
 * @param {TriggerOptions} options
 * @param {number | null | undefined} timeout_ms
 * @returns {TriggerOptions}
 */
function withTimeout(options, timeout_ms) {
    if (timeout_ms !== undefined && timeout_ms !== null && timeout_ms > 0) options.timeoutMs = timeout_ms;
    return options;
}

/**
 * @param {string | undefined} name
 * @returns {number}
 */
function parseStepIndex(name) {
    if (typeof name !== "string" || name.indexOf("recipe:") !== 0) return -1;
    const rest = name.substring("recipe:".length);
    const index = Number(rest);
    if (!Number.isInteger(index) || index < 0) return -1;
    return index;
}

/**
 * 把一份 `Step[]` 翻成 `PluginModule` 形状的 callback 链。
 *
 * @param {{origin?: string, steps: RecipeStep[]} | RecipeStep[]} plan `stepsJson()` 的结果（或裸的 steps 数组）。
 * @returns {RecipeChain} 宿主只读 `start` / `onTrigger`。
 */
export function runRecipe(plan) {
    if (plan === null || plan === undefined) {
        throw new Error("recipe: 没有 plan（需要 recipe_json 的 {origin, steps}）");
    }
    const steps = Array.isArray(plan) ? plan : plan.steps;
    if (!Array.isArray(steps) || steps.length === 0) {
        throw new Error("recipe: steps 不是非空数组（parser 产出的配方不会为空）");
    }
    const origin = Array.isArray(plan) || plan.origin === undefined ? "(memory)" : String(plan.origin);

    // 进度（闭包；模块只求值一次 ⇒ 跨回调存活）。★ 只作读数与链的游标，⛔ 不参与门判定。
    let cursor = 0;   // 下一条要消费的指令下标
    let applied = 0;  // 已消费的指令数
    let armed = "";   // 当前 arm 的 trigger 名（调试 / 失败点名用）

    function progressText() {
        return "applied=" + applied + " pending=" + (steps.length - applied) + " origin=" + origin;
    }

    // 一步的**翻译**（只对会"等"的指令：门 / wait）。
    /**
     * @param {RunCtx} ctx
     * @param {number} index
     * @returns {Trigger}
     */
    function armStep(ctx, index) {
        const step = steps[index];
        const name = "recipe:" + index;
        armed = name;
        if (step.kind === KIND_AWAIT_SCRIPT) {
            return ctx.trigger.afterScript(step.script, withTimeout({ name: name }, step.timeoutMs));
        }
        if (step.kind === KIND_AWAIT_POSITION) {
            return ctx.trigger.afterPosition(step.script, step.posFrom, step.posTo,
                                             withTimeout({ name: name }, step.timeoutMs));
        }
        if (step.kind === KIND_WAIT) {
            // ★ 宿主墙钟（NON-DIFFABLE）；`ms` 是延迟本身，⛔ 不叠 timeout。
            return ctx.trigger.afterDelay(step.waitMs, { name: name });
        }
        throw new Error("recipe: 第 " + (index + 1) + " 步（" + step.kind + "）不是可 arm 的门");
    }

    // 一条写指令 → `ctx.input.*`。返回 false = 已 `ctx.fail`（调用方立刻停链）。
    /**
     * @param {RunCtx} ctx
     * @param {RecipeStep} step
     * @param {number} index
     * @returns {boolean}
     */
    function submitWrite(ctx, step, index) {
        const name = "recipe:" + index;
        /** @type {InputReceipt | null} */
        let receipt = null;
        if (step.kind === KIND_CLICK) {
            receipt = ctx.input.click(step.x, step.y, step.button);
        } else if (step.kind === KIND_MOUSE_MOVE) {
            receipt = ctx.input.move(step.x, step.y);
        } else if (step.kind === KIND_MOUSE_DOWN) {
            // ⚠ 已知缺口 ①：C1 的 executor 不施加显式 down/up ⇒ 它会在队首头阻。
            receipt = ctx.input.down(step.button);
        } else if (step.kind === KIND_MOUSE_UP) {
            receipt = ctx.input.up(step.button);
        } else {
            throw new Error("recipe: 未知写指令 " + step.kind);
        }
        if (receipt === null || receipt === undefined || receipt.accepted !== true) {
            const code = receipt !== null && receipt !== undefined && receipt.code !== undefined
                             ? String(receipt.code)
                             : "没有回执 / 没有 code";
            ctx.fail(name, "recipe: 第 " + (index + 1) + " 步（" + step.kind + "）被宿主拒收：" + code);
            return false;
        }
        return true;
    }

    // 从 `cursor` 起消费指令：非阻塞的一路走到底，遇到门 / wait 就 arm 并返回。
    /**
     * @param {RunCtx} ctx
     * @returns {Trigger | Trigger[]}
     */
    function advance(ctx) {
        for (;;) {
            if (cursor >= steps.length) {
                armed = "";
                // ★ 完成 = **正常收口**（`finish(0)` 结束本次 Run；⛔ 不 `quit`：那会把归因改成
                //   `plugin: quit`，而"配方走完"不是"请求宿主停机"）。
                ctx.finish(0, "recipe: done " + progressText());
                return [];
            }
            const step = steps[cursor];
            if (isGateKind(step.kind) || step.kind === KIND_WAIT) {
                return armStep(ctx, cursor);
            }
            if (isWriteKind(step.kind)) {
                const index = cursor;
                if (!submitWrite(ctx, step, index)) {
                    // 终态异常：把余下的链也停掉（`ctx.fail` 已让宿主清 armed / staged input）。
                    cursor = steps.length;
                    applied = index + 1;
                    armed = "";
                    return [];
                }
                cursor = index + 1;
                applied = index + 1;
                continue;
            }
            if (step.kind === KIND_PROFILE_BEGIN) {
                ctx.profile.begin(step.label);
                cursor += 1;
                applied = cursor;
                continue;
            }
            if (step.kind === KIND_PROFILE_END) {
                ctx.profile.end();
                cursor += 1;
                applied = cursor;
                continue;
            }
            if (step.kind === KIND_QUIT) {
                cursor += 1;
                applied = cursor;
                armed = "";
                const remaining = steps.length - cursor;
                // ★ 旧执行器：`quit` 之后还有指令 ⇒ 没走完（`outcome()==Running` ⇒ exit 5）；
                //   `quit` 是最后一条 ⇒ `Done` ⇒ 0。⇒ 退出码按**剩余条数**映射（⛔ 不恒 5）。
                ctx.finish(remaining > 0 ? INCOMPLETE_EXIT_CODE : 0,
                           "recipe: quit at step " + cursor + "/" + steps.length +
                               (remaining > 0 ? "（后面还有 " + remaining + " 条没消费 ⇒ exit 5）" : "") +
                               "；" + progressText());
                // `ctx.quit` = 宿主 stop_requested（旧执行器同一条语义的**另一个**机制：WM_QUIT）。
                ctx.quit("recipe: quit（" + origin + "）");
                return [];
            }
            // 未知 kind：`StepKind` 是闭集合 ⇒ 走到这里说明 JSON / 闭集合对不上，必须响亮。
            ctx.fail("recipe:" + cursor,
                     "recipe: 未知指令 `" + step.kind + "`（第 " + (cursor + 1) + " 步）—— `StepKind` 是闭集合");
            cursor = steps.length;
            applied = steps.length;
            armed = "";
            return [];
        }
    }

    return {
        // 宿主在第一个读点之后调一次（`PluginHost` 的 D-C2-10 口径）。
        start(ctx) {
            cursor = 0;
            applied = 0;
            armed = "";
            ctx.log("info", "recipe: start " + origin + "（steps=" + steps.length + "）");
            return advance(ctx);
        },

        // 一次回调 = 一步被消费（门成立 / wait 到期 / 门超时）。
        onTrigger(ctx, ev) {
            if (ev.kind === "timeout") {
                const index = parseStepIndex(ev.name);
                const step = index >= 0 && index < steps.length ? steps[index] : null;
                const text = step === null ? "?" : stepTextOf(step);
                armed = "";
                // ★ 门超时 = 没走完 ⇒ exit 5（旧执行器 `TimedOut` 的同一个数；⛔ 不改共享判据，
                //   边界那 1 ms 差异见合同 `:1182-1184`）。
                ctx.finish(INCOMPLETE_EXIT_CODE,
                           "recipe: timeout at step " + (index + 1) + "/" + steps.length + "：" + text +
                               "；" + progressText());
                return [];
            }
            const index = parseStepIndex(ev.name);
            if (index !== cursor) {
                ctx.fail(ev.name === undefined ? "recipe:?" : String(ev.name),
                         "recipe: 触发了不是当前步骤的 trigger（fired=" + index + " armed=" + cursor + "）");
                cursor = steps.length;
                applied = steps.length;
                armed = "";
                return [];
            }
            cursor = index + 1;
            applied = cursor;
            armed = "";
            return advance(ctx);
        },
    };
}
