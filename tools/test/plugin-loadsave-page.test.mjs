/** @env pure @kind gate @why `loadSave` 的翻页等待窗又缩回"比一次点击还短"：正常点击会被当成丢了、真丢了又救不回来（11-24 的 `page not reached (target 0, still 1)`） */
/**
 * tools/test/plugin-loadsave-page.test.mjs —— `loadSave` **翻页链**的判定逻辑（纯 JS，无 guest、无 exe）
 *
 * 为什么需要它（缺口）：
 *   · `plugin-headless` 的 `plugin/load-save-e2e` **故意**用 `slot=15`（页 1 第 5 行 = 进屏那一页）⇒
 *     `calls ∈ {6,7}` 只描述"不翻页"那条链；`plugin-runtime` 的脚本夹具直接把 `0x138e` 种成目标页
 *     ⇒ **翻页那条链（`load:pg:*`）在两道门里一次也没跑过**。
 *   · 于是 `PAGE_STALL_LIMIT` 从 60 起就没有任何守卫：60 个读点 ≈ 164 ms，而一次箭头点击要跨
 *     **两个完整 guest 回调**（闸门②′）才抬起 ⇒ 真机上页号要 76–77 个读点才变（读数出处：
 *     `apps/emulator-lift/scripts/plugins/lib/save.js` 文件头 §2026-11-25）。
 *   ⇒ 本用例用一个**假的 ctx + 假的 guest** 把那条链跑起来，判三件事（都是"变了就是 bug、而且变了
 *     是静默的"）：
 *       ① 正常路径（第一下点击被消费）⇒ **只允许一次**箭头点击（多一次 = 页会被多翻一格 → 点到空槽）；
 *       ② 第一下被吞 ⇒ 重试点击必须**真的能救回来**（`turnr` 的预算要容得下"重试自己的落地延迟"）；
 *       ③ 两条都要求最终真的到达目标页（`load:page-ready`），⛔ 不是"没超时就算过"。
 *
 * ★ 模型参数 `CLICK_LATENCY_READS = 76` 是**模型参数**（出处 = 上面那条读数），⛔ 不是"断言当前取值"：
 *   本文件的断言全在**链的判定**上（点了几次箭头 / 有没有到目标页），没有一条断言游戏读数。
 *   旧值 60 的红法：①在正常路径上会多出第 2 次点击；②重试点击在 60+76=136 才落地，而 `turnr` 的
 *   60 个读点在 121 就用完了 ⇒ 停在 1。
 *
 * 运行：`node --test tools/test/plugin-loadsave-page.test.mjs`（`@env pure` ⇒ 也在 `pnpm test` 里）。
 *   变异自检：`node tools/mutate-check.mjs --match loadsave-page`。
 */
import assert from 'node:assert/strict';
import test from 'node:test';

import { loadSave } from '../../apps/emulator-lift/scripts/plugins/lib/save.js';

/** 模型参数：一次箭头点击从"提交"到"页号变"要跨过的读点数（读数出处见文件头）。 */
const CLICK_LATENCY_READS = 76;
/** 翻页链要读的页号 global（与 `guest-globals.js` 的 `SAVE_PAGE_GLOBAL_ID` 同一个数；本用例只做假读数）。 */
const SAVE_PAGE_GLOBAL_ID = 0x138e;
/** `SAVE.BIN` 的回调体（`load:page-ready` 的门区间；真值来自 `script-positions.js`）。 */
const SAVE_BODY = [0x1104, 0x11c4];
const SCRIPT_SAVE = 'SAVE.BIN';
/** 目标槽 = 9（页 0 第 9 行）⇒ 页 1 进屏时必须翻到页 0。 */
const SLOT = 9;
const TARGET_PAGE = 0;
/** 假 guest 的上限（远大于任何一次模型的落地时刻；超了就是"链没收敛"）。 */
const MAX_READS = 6000;

/**
 * 一个**只有本用例需要的那几面**的 `RunCtx`：
 *   · `read.global.get` ⇒ 假页号；`input.click/move` ⇒ 记下来并排"页号什么时候变"；
 *   · `trigger.*` ⇒ 纯数据对象（由下面的 driver 求值，不实现 timeout —— 本用例不给 `timeoutMs` 赋真时间）。
 *
 * @param {{ startPage: number, latency: number, swallowFirstArrowClick: boolean }} model
 */
function makeWorld(model) {
    let page = model.startPage;
    let read = 0;
    /** 已提交的 click（`{x,y,at}`）。 */
    const clicks = [];
    /** 待生效的翻页（`{at, delta}`）。 */
    const flips = [];
    let swallowPending = model.swallowFirstArrowClick;
    const logs = [];
    /** @type {{code:number, reason:string}|null} */
    let finished = null;
    /** @type {{name:string, reason:string}|null} */
    let failed = null;

    const ctx = {
        params: {},
        now: { script: SCRIPT_SAVE, pos: SAVE_BODY[0], point: 'peekMsg', readIndex: 0 },
        read: {
            global: {
                get(kind, id) {
                    if (kind === 'int' && id === SAVE_PAGE_GLOBAL_ID) return page;
                    throw new Error(`假 ctx 只认 int ${SAVE_PAGE_GLOBAL_ID}（收到 ${kind} ${id}）`);
                },
            },
        },
        input: {
            move() {
                /* 本用例不判悬停；`load:page-ready` 之后那一步会 move */
            },
            click(x, y) {
                clicks.push({ x, y, at: read });
                const arrow = x === 45 || x === 1235;
                if (!arrow) return;
                if (y !== 357) throw new Error(`箭头点击的 y 不是 357（收到 ${y}）`);
                if (swallowPending) {
                    // ★ 模型：第一下被吞（= 真机上偶尔发生的那一下；⛔ 不是"点击没提交"）
                    swallowPending = false;
                    return;
                }
                flips.push({ at: read + model.latency, delta: x === 45 ? -1 : 1 });
            },
        },
        trigger: {
            afterScript(script, opts) {
                return { kind: 'afterScript', script, name: opts.name };
            },
            afterPosition(script, lo, hi, opts) {
                return { kind: 'afterPosition', script, lo, hi, name: opts.name };
            },
            afterDelay(ms, opts) {
                return { kind: 'afterDelay', ms, name: opts.name };
            },
            atReadPoint(point, maxFires, opts) {
                return { kind: 'atReadPoint', point, maxFires, name: opts.name };
            },
        },
        log(level, message) {
            logs.push(`${level}: ${message}`);
        },
        finish(code, reason) {
            finished = { code, reason };
        },
        fail(name, reason) {
            failed = { name, reason };
        },
    };

    /** 世界推进到下一个读点（先结算到期的翻页 = 页号真的变了，再让 driver 判 trigger）。 */
    function advance() {
        read += 1;
        for (let i = 0; i < flips.length; ) {
            if (flips[i].at <= read) {
                page = ((page + flips[i].delta) % 90 + 90) % 90;
                flips.splice(i, 1);
            } else {
                i += 1;
            }
        }
        ctx.now.readIndex = read;
        ctx.now.pos = SAVE_BODY[0];
    }

    /** 这个 trigger 在本读点成不成立（`any` 的 atReadPoint 恒成立；本用例不用 afterDelay）。 */
    function due(trigger) {
        switch (trigger.kind) {
            case 'afterScript':
                return trigger.script === SCRIPT_SAVE;
            case 'afterPosition':
                return trigger.script === SCRIPT_SAVE && trigger.lo <= ctx.now.pos && ctx.now.pos < trigger.hi;
            case 'atReadPoint':
                return true;
            case 'afterDelay':
                return false;
            default:
                throw new Error(`driver 不认识 trigger.kind=${trigger.kind}`);
        }
    }

    return {
        ctx,
        clicks,
        logs,
        get page() {
            return page;
        },
        get read() {
            return read;
        },
        get finished() {
            return finished;
        },
        get failed() {
            return failed;
        },
        advance,
        due,
    };
}

/**
 * 把 `loadSave(SLOT)` 的链跑到"页到了"或"链自己收口"为止。
 *
 * @param {{ startPage?: number, latency?: number, swallowFirstArrowClick?: boolean }} [model]
 * @returns {{ reachedPage: boolean, arrowClicks: number, page: number, reads: number, finished: any, failed: any }}
 */
function drivePageChain(model = {}) {
    const world = makeWorld({
        startPage: model.startPage === undefined ? 1 : model.startPage,
        latency: model.latency === undefined ? CLICK_LATENCY_READS : model.latency,
        swallowFirstArrowClick: model.swallowFirstArrowClick === true,
    });
    const chain = loadSave(SLOT, { expect: ['FIELD.BIN'] });
    /** @type {any[]} */
    let armed = normalize(chain.start(world.ctx));
    let reachedPage = false;
    let rowClicked = false;
    while (
        world.read < MAX_READS &&
        world.finished === null &&
        world.failed === null &&
        !reachedPage &&
        !rowClicked
    ) {
        world.advance();
        const trigger = armed.find((t) => world.due(t));
        if (trigger === undefined) continue;
        const ev = { kind: 'trigger', name: trigger.name, point: 'peekMsg', script: SCRIPT_SAVE, pos: world.ctx.now.pos };
        armed = normalize(chain.onTrigger(world.ctx, ev));
        if (armed.some((t) => t.name === 'load:page-ready')) reachedPage = true;
        // 已经在目标页（不需要翻页）时链直接走 `load:row` → `load:sbunki`（页链的出口）
        if (armed.some((t) => t.name === 'load:sbunki')) rowClicked = true;
    }
    const arrowClicks = world.clicks.filter((c) => c.x === 45 || c.x === 1235).length;
    const rowClicks = world.clicks.filter((c) => c.x === 300).length;
    return {
        reachedPage,
        rowClicked,
        arrowClicks,
        rowClicks,
        page: world.page,
        reads: world.read,
        finished: world.finished,
        failed: world.failed,
    };
}

/** @param {any} value @returns {any[]} */
function normalize(value) {
    if (value === null || value === undefined) return [];
    return Array.isArray(value) ? value : [value];
}

test('loadsave-page/normal：第一下点击被消费 ⇒ 到达目标页，且**只点一次**箭头（多点一次会把页多翻一格）', () => {
    const r = drivePageChain();
    assert.equal(r.failed, null, `链自己 fail 了：${JSON.stringify(r.failed)}`);
    assert.equal(r.finished, null, `链提前收口（finish）：${JSON.stringify(r.finished)}`);
    assert.equal(r.reachedPage, true, `没到目标页（page=${r.page}, reads=${r.reads}）`);
    assert.equal(r.page, TARGET_PAGE, `页号不是目标页（page=${r.page}）`);
    assert.equal(
        r.arrowClicks,
        1,
        `正常路径点了 ${r.arrowClicks} 次箭头 —— 第 2 次是"等待窗比点击自身还短"造成的重试，` +
            '它在页号已经变了之后才落地 ⇒ 会把页又翻一格（0 → 89）⇒ 点行点到空槽。',
    );
});

test('loadsave-page/first-click-swallowed：第一下被吞 ⇒ 重试必须真的能救回来（重试自己的落地延迟也算在预算里）', () => {
    const r = drivePageChain({ swallowFirstArrowClick: true });
    assert.equal(r.failed, null, `链自己 fail 了：${JSON.stringify(r.failed)}`);
    assert.equal(
        r.reachedPage,
        true,
        `第一下被吞之后没被重试救回来（page=${r.page}, reads=${r.reads}, finish=` +
            `${JSON.stringify(r.finished)}）—— 重试点击自己也要 ${CLICK_LATENCY_READS} 个读点才落地，` +
            '`turnr` 的等待窗必须容得下它。',
    );
    assert.equal(r.page, TARGET_PAGE, `页号不是目标页（page=${r.page}）`);
    assert.equal(r.arrowClicks, 2, `应该恰好"第一次 + 重试一次"两次箭头点击（实际 ${r.arrowClicks}）`);
});

test('loadsave-page/already-on-target-page：进屏就在目标页 ⇒ 一次箭头都不点（别把"不需要翻页"也翻一格）', () => {
    const r = drivePageChain({ startPage: TARGET_PAGE });
    assert.equal(r.failed, null, `链自己 fail 了：${JSON.stringify(r.failed)}`);
    assert.equal(r.rowClicked, true, `没走到"点行"那一步（reads=${r.reads}）`);
    assert.equal(r.arrowClicks, 0, `已经在目标页却点了 ${r.arrowClicks} 次箭头`);
    assert.equal(r.rowClicks, 1, `应该恰好点一次行（实际 ${r.rowClicks}）`);
    assert.equal(r.page, TARGET_PAGE, `页号不是目标页（page=${r.page}）`);
});
