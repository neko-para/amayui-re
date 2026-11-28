// scripts/plugins/lib/gates.ts —— **门的薄封装**（v2：`ctx.once` + 一条失败收口 + 多臂等待）
//
// 为什么有一层（2026-11-25 v1 起；2026-11-28 迁 v2）：每道门的**调用形状**逐字相同
//   （`{script, lo, hi}` / `{script}` + `timeoutMs` + 失败文案），"超时 ⇒ finish(5)"也每处相同。
//   复制的是**形状**，不是数值 —— 区间 / 超时按文件、按场景各不同（属读数）⇒ 本文件只收形状。
//
// ⛔ 三条边界（别把工具做成第二份真源）：
//   · **不 import `./script-positions.ts`**：区间数字由调用方给（那张表是位置读数的唯一真源，
//     本文件一格都不写）；门的**名字**（`boot:title` / `load:save` / `scenario:fellow:0` …）
//     同样由调用方给 —— 名字是失败文案里的定位标签（`<label>: gate timeout (<name>)`）。
//   · **不 import `./chains.ts`**（该文件已删）：v2 没有"按名字路由"，组合就是顺序 `await`。
//   · **不改判据**：超时只做 `ctx.finish(5, …)`（⛔ 不 `quit` —— 那会把归因改成 `plugin: quit`）。
//     ★ P0 起 `ctx.finish` 在 task continuation / 泵里也可用；超时后**再 throw** 让 task 结束 ——
//       Run 已到终态，`once-timeout` 的 fail 不会覆盖 `finished exit=5`（`runTerminal` 早退）。
//
// ★ 文案（前缀）逐字保留 v1：`<label>: <phrase> (<name>)`。`label` 省略 = 不带前缀。
//   `plugin_e2e.cpp` / `plugin_runtime.cpp` 的负断言与排障口径都按前缀分链（`load-save: gate timeout` …）。
//   ⛔ 不要在这里"统一文案"。
//
// ★ `awaitWhere` 用 `ctx.once`：`{script,lo,hi}` 的语义 = **区间 [lo,hi) 里第一条指令**
//   （v1 `afterPosition` 的 `mode:"once"`），`{script}` = **该脚本下一条指令**（v1 `afterScript`）。
//   ★ 一轮 **per-callback** 的采样用 `{script, pos: <回调体入口>}`：v1 的 `atReadPoint("any")`
//     是"下一个读点"（≈ 每个 guest 回调一次），而 `{script}` 是"下一条指令"（频率高一个量级）
//     ⇒ 页链的"等 N 拍"预算不能直接套到 `{script}` 上（见 `save.ts` 的 `sampleSave`）。

import type { AtWhere, RunCtx, Tick } from "@amayui/lift-plugin";

/**
 * 超时 = **响亮收口**：`ctx.finish(5, "<label>: <phrase> (<name>)")`。
 *
 * ★ 为什么是 `finish(5)`：那是历史 exit 5 的映射，也是 `plugin_e2e` / 排障沿用的归因；
 *   ⛔ 不要用 `ctx.quit` —— 归因会变成 `plugin: quit`，看起来像"用户关窗口"。
 *
 * @param ctx RunCtx。
 * @param label 前缀（`boot` / `load-save` / `scenario` / `scenario-fellow` / …）；空串 = 不带。
 * @param name 门的名字（v1 的 trigger name，逐字保留在文案里）。
 * @param phrase 超时短语；缺省 `gate timeout`（`scenario-save10` 的历史文案是 `timeout`）。
 */
export function failOnTimeout(ctx: RunCtx, label: string, name: string, phrase = "gate timeout"): void {
    ctx.finish(5, `${label === "" ? "" : label + ": "}${phrase} (${name})`);
}

/**
 * 等一个 `AtWhere`；超时 ⇒ `finish(5, …)` 然后**重新抛出**（task 结束；Run 已是终态）。
 *
 * @param ctx RunCtx。
 * @param where 与 `ctx.once` 完全相同的形态。
 * @param name 门的名字（只用于失败文案）。
 * @param timeoutMs 失败截止（`ctx.once` 要求 > 0）。
 * @param label 失败文案前缀。
 * @param phrase 超时短语。
 */
export async function awaitWhere(
    ctx: RunCtx,
    where: AtWhere,
    name: string,
    timeoutMs: number,
    label: string,
    phrase = "gate timeout",
): Promise<Tick> {
    try {
        return await ctx.once(where, { timeoutMs });
    } catch (err) {
        failOnTimeout(ctx, label, name, phrase);
        throw err;
    }
}

/**
 * 等"某个脚本的 `[lo,hi)` 位置区间"里的**第一条指令**（v1 `afterPosition` 的 once 语义）。
 *
 * @param ctx RunCtx。
 * @param script **marker 文件名**（例 `FIELD.BIN`；宿主是精确相等，⛔ 不剥 `.BIN`）。
 * @param lo 区间下界（含）。
 * @param hi 区间上界（不含）。
 * @param name 门的名字（失败文案里的 `(...)`）。
 * @param timeoutMs 门超时（毫秒）。
 * @param label 失败文案前缀。
 * @param phrase 超时短语。
 */
export function awaitPosition(
    ctx: RunCtx,
    script: string,
    lo: number,
    hi: number,
    name: string,
    timeoutMs: number,
    label: string,
    phrase = "gate timeout",
): Promise<Tick> {
    return awaitWhere(ctx, { script, lo, hi }, name, timeoutMs, label, phrase);
}

/**
 * 等"脚本身份"（v1 `afterScript` 的等价物）：该脚本的**下一条指令**。
 *
 * @param ctx RunCtx。
 * @param script **marker 文件名**（例 `SETTOOLLIST.BIN`）。
 * @param name 门的名字。
 * @param timeoutMs 门超时（毫秒）。
 * @param label 失败文案前缀。
 * @param phrase 超时短语。
 */
export function awaitScript(
    ctx: RunCtx,
    script: string,
    name: string,
    timeoutMs: number,
    label: string,
    phrase = "gate timeout",
): Promise<Tick> {
    return awaitWhere(ctx, { script }, name, timeoutMs, label, phrase);
}

/** 一条"多臂比赛"的臂：命中第一条就赢，其余臂由 `ctx.waitAny` 立刻撤掉。 */
export interface Arm {
    /** 订阅条件。 */
    where: AtWhere;
    /** 调用方自己的标签（用来判是谁赢了）。 */
    tag: string;
}

/** 命中结果。 */
export interface ArmHit {
    /** 命中那一拍的 Tick（写面仍有效：本入口的 drain 内）。 */
    tick: Tick;
    /** 赢的那条臂的 `tag`。 */
    tag: string;
}

/**
 * 同时等多条臂，**第一条命中** ⇒ resolve（第一条命中的臂；其余臂的临时 hook 立刻撤掉）。
 *
 * ★ 实现 = `ctx.waitAny(arms.map(a => a.where), { timeoutMs })`（S2 新增原语；P1 复审 S2 的修法）：
 *   · **第一条命中胜出**，其余臂的临时 hook **立刻撤掉** —— 这正是 `Promise.race([ctx.once(...)])`
 *     做不到的事：`once` 没有句柄 / 不能撤，输的那条会一直留在 `hasPendingWork` 里，直到命中或
 *     **timeout ⇒ fail Run**；
 *   · `timeoutMs` **必填 > 0**（wall-clock 失败截止）：到期 reject（reason 列出全部 arms）⇒ 泵
 *     drain 后 fail Run（与 `ctx.once` 的 timeout **同一条**"失败截止"语义）；
 *   · 命中 / 超时 / Run 收口 / `hook.clear()` 都把整组撤干净（整组被外部清掉 ⇒ reject），
 *     ⛔ 不会留下永不 settle 的 Promise（那会让 Run 只能等 `--time-limit`）。
 *
 * ★ 失败文案逐字保留 v1（`<label>: gate timeout (<name>)`；`<name>` = **第一条臂**的 tag）：v1 里
 *   expect 的每条臂各自带 timeout，等不到时打在**最先超时**的那条（= 注册序第一条）⇒ 这里取
 *   `arms[0].tag` 是同一读法（单 expect 时就是 `load:expect:<脚本名>`，与 v1 逐字相同）。
 *   ★ catch 不区分"超时 / 被 `hook.clear()` 撤掉"：与 `awaitWhere` 同一条口径（catch-all 归因，
 *     见复审 S3 备注；实际调用点没有 `hook.clear`）。
 *
 * @param ctx RunCtx。
 * @param arms 至少一条。
 * @param timeoutMs wall-clock 失败截止（必填 > 0）。
 * @param label 失败文案前缀（`load-save`）。
 */
export async function waitFirst(
    ctx: RunCtx,
    arms: readonly Arm[],
    timeoutMs: number,
    label: string,
): Promise<ArmHit> {
    if (!Array.isArray(arms) || arms.length === 0) {
        throw new Error("waitFirst: arms 至少 1 条");
    }
    try {
        const hit = await ctx.waitAny(
            arms.map((arm) => arm.where),
            { timeoutMs },
        );
        const arm = arms[hit.index];
        if (arm === undefined) {
            throw new Error(`waitFirst: ctx.waitAny 返回了非法 index ${String(hit.index)}`);
        }
        return { tick: hit.tick, tag: arm.tag };
    } catch (err) {
        // 超时（或被 clear）= 失败截止：与 `awaitWhere` 同一条 —— 先 `finish(5)`（Run 已终态 ⇒
        // 泵随后的 timeout fail 不覆盖读数），再 throw 让 task 结束。
        failOnTimeout(ctx, label, arms[0].tag);
        throw err;
    }
}
