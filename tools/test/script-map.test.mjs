/** @env pure @kind gate @why 插件的 lib 里又出现裸 `0x…` 位置字面量（常量表被绕过 / 新读数写死在消费方）—— 换一处取值就没人知道该改哪份，且门会静默不命中 */
/**
 * tools/test/script-map.test.mjs —— "脚本位置 / guest global 只有一个数字真源"的守卫
 *
 * 红了意味着什么（三件都是"变了就是 bug、而且变了**是静默的**"）：
 *   ① `apps/emulator-lift/scripts/plugins/` **两个目录**（根上的入口 `*.ts` 与 `lib/*.ts` 的库）里
 *      出现了 `0x…` 字面量 —— 除了两张常量表
 *      （`lib/script-positions.ts` = 脚本内位置；`lib/guest-globals.ts` = guest global 的 id / 位）。
 *      危害不是"难读"：同一段区间被写死在两处之后，改一处读数另一处**照样跑**，只是门不命中 /
 *      点不进窗口 ⇒ 表现成"偶发 gate timeout"，没人能看出是哪份数字过期了。
 *   ② 常量表里的导出**重名**（ES module 同文件重名是语法错，但复制粘贴时很容易造成"以为用了新的、
 *      其实是旧的"的假象；这里按名字查一遍，报错时点名）。
 *   ③ 两个已知消费方（`lib/save.ts` / 根上的 `scenario-save07-battle.ts`）不再 import 常量表 ——
 *      那意味着它们把值写成了十进制/字符串形式（钻字面量检查的空子）或者根本没接线。
 *
 * ★ **S1（2026-11-25）起扫两个目录**：入口（`scenario-*.ts` / `load-save.ts`）搬到了 plugin 根，
 *   库（`save.ts` / `script-positions.ts` / `guest-globals.ts` / …）留在 `lib/`。
 *   只扫其中一个 ⇒ 另一个里的裸 `0x…` **静默漏检**（本文件的地板断言就是防这个）。
 * ★ **必须剥注释再扫**：`save.ts` / `scenario-save07-battle.ts` 的头注释里印着同一批区间
 *   （那是给人看的读数台账）—— 朴素扫描会在"已经改对了"的树上误报。`stripComments()` 同时
 *   认得字符串字面量（`//` 在字符串里不是注释），所以 `save.ts` 里 `"0x" + text` 这类写法不会被当成注释吃掉。
 * ★ 允许列表是**文件名**（不是 glob）：新增一张常量表要显式改这里 —— 那正是希望有人停下来想一下的地方。
 *
 * 怎么加一条新位置：把值加进 `lib/script-positions.ts`（带用途 + 读数出处），消费方 `import`。
 *   guest global 的 id / 位 ⇒ 加进 `lib/guest-globals.ts`。
 * 运行：`node --test tools/test/script-map.test.mjs`（`@env pure` ⇒ 也在 `pnpm test` 里）。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

import { REPO_ROOT } from '../lib/paths.mjs';

const PLUGIN_DIR = path.join(REPO_ROOT, 'apps', 'emulator-lift', 'scripts', 'plugins');
const LIB_DIR = path.join(PLUGIN_DIR, 'lib');

/** 唯一允许出现 `0x…` 的文件（各自是**一张表的真源**；都在 `lib/` 下）。 */
const HEX_ALLOWED = new Set(['script-positions.ts', 'guest-globals.ts']);

/** 必须 import 常量表的消费方（③）；`dir` 区分根上的入口与 `lib/` 的库。 */
const CONSUMERS = [
    { dir: LIB_DIR, file: 'save.ts', table: './script-positions.ts' },
    { dir: PLUGIN_DIR, file: 'scenario-save07-battle.ts', table: './lib/script-positions.ts' },
    { dir: PLUGIN_DIR, file: 'scenario-save07-battle.ts', table: './lib/guest-globals.ts' },
];

/**
 * 剥掉 `//…` 与 `/*…*\/` 注释，**保留字符串字面量原样**（包括字符串里的 `//`）。
 * @param {string} text
 * @returns {string} 与原文等长的代码骨架（换行保留 ⇒ 行号仍可算）
 */
export function stripComments(text) {
    let out = '';
    let i = 0;
    while (i < text.length) {
        const c = text[i];
        const d = i + 1 < text.length ? text[i + 1] : '';
        if (c === '/' && d === '/') {
            while (i < text.length && text[i] !== '\n') i += 1;
            continue;
        }
        if (c === '/' && d === '*') {
            i += 2;
            while (i < text.length && !(text[i] === '*' && text[i + 1] === '/')) {
                if (text[i] === '\n') out += '\n';  // 行号不漂
                i += 1;
            }
            i += 2;
            continue;
        }
        if (c === '"' || c === "'" || c === '`') {
            const quote = c;
            out += c;
            i += 1;
            while (i < text.length) {
                if (text[i] === '\\') {
                    out += text[i] + (i + 1 < text.length ? text[i + 1] : '');
                    i += 2;
                    continue;
                }
                out += text[i];
                if (text[i] === quote) {
                    i += 1;
                    break;
                }
                i += 1;
            }
            continue;
        }
        out += c;
        i += 1;
    }
    return out;
}

/** 扫一段代码里的 `0x…` 字面量（返回 `[{literal, line}]`；`code` 必须已剥注释）。 */
export function findHexLiterals(code) {
    const found = [];
    const re = /0[xX][0-9A-Fa-f]+/g;
    let m;
    while ((m = re.exec(code)) !== null) {
        const line = code.slice(0, m.index).split('\n').length;
        found.push({ literal: m[0], line });
    }
    return found;
}

/**
 * 两个目录下的全部 `.ts`：`{ rel, name, file }`（`rel` 是相对 `scripts/plugins/` 的展示路径）。
 * ★ 读不到目录 ⇒ 直接抛 —— 路径错了不许变成"0 个文件 ⇒ 恒绿"。
 */
function pluginFiles() {
    const entries = [];
    for (const [dir, prefix] of [
        [LIB_DIR, 'lib/'],
        [PLUGIN_DIR, ''],
    ]) {
        for (const name of fs.readdirSync(dir)) {
            if (!name.endsWith('.ts')) continue;
            entries.push({ rel: prefix + name, name, file: path.join(dir, name) });
        }
    }
    return entries.sort((a, b) => a.rel.localeCompare(b.rel));
}

test('script-map/scanner-strips-comments：注释里的 0x 不算，代码里的 0x 一定算（否则本守卫会恒绿或误报）', () => {
    const snippet = [
        '// 注释里的 0x0B18 与 /* 0x1234 */ 都不该算',
        'const a = "not a literal 0x";',  // 后面没有十六进制位 ⇒ 不是字面量
        'const b = 0x0b18;',
        'const url = "http://example.invalid/x";',  // 字符串里的 // 不是注释
        'const c = 0xFF; // 行尾注释 0x999',
    ].join('\n');
    const code = stripComments(snippet);
    const found = findHexLiterals(code).map((x) => x.literal);
    assert.deepEqual(found, ['0x0b18', '0xFF'], `剥注释后的扫描结果不对：${JSON.stringify(found)}`);
    // 字符串没被当成注释吃掉（否则上面 `const url` 之后的代码会整段消失）
    assert.ok(code.includes('const c = 0xFF;'), '剥注释把字符串里的 // 当成了注释');
    assert.ok(!code.includes('0x999'), '行尾注释没被剥掉');
});

test('script-map/no-bare-hex：plugins 根 + lib/ 里除两张常量表外不许出现 `0x…` 字面量（剥注释后）', () => {
    const files = pluginFiles();
    // 地板：两个目录都真的扫到了（路径错 / 后缀变 / 漏扫一个目录 ⇒ 不许空扫描变绿）。
    assert.ok(files.length >= 7, `只扫到 ${files.length} 个 .ts（${PLUGIN_DIR}）—— 路径 / 后缀变了？`);
    const names = new Set(files.map((f) => f.rel));
    assert.ok(names.has('lib/save.ts'), `lib/ 里没有 save.ts：${[...names].join(', ')}`);
    assert.ok(
        names.has('lib/script-positions.ts') && names.has('lib/guest-globals.ts'),
        `lib/ 里没有常量表：${[...names].join(', ')}`,
    );
    // ★ 根上的入口必须真的被扫到（S1 的移动就是把入口搬到这里；漏扫 ⇒ 整个根目录静默失守）。
    assert.ok(
        names.has('scenario-save07-battle.ts') && names.has('load-save.ts'),
        `plugins 根上没扫到已知入口：${[...names].join(', ')}`,
    );

    const violations = [];
    for (const entry of files) {
        if (HEX_ALLOWED.has(entry.name)) continue;
        const code = stripComments(fs.readFileSync(entry.file, 'utf8'));
        for (const hit of findHexLiterals(code)) {
            violations.push(`${entry.rel}:${hit.line}  ${hit.literal}`);
        }
    }
    assert.deepEqual(
        violations,
        [],
        '插件脚本里出现了裸 `0x…` 字面量（位置 / global 只有一个数字真源）：\n  ' +
            `${violations.join('\n  ')}\n` +
            '要么 `import { … } from "./lib/script-positions.ts"`（guest global 走 "./lib/guest-globals.ts"；' +
            '`lib/` 内部用同目录 "./script-positions.ts"），' +
            '要么把新值加进 `lib/script-positions.ts`。⛔ 不要为了方便就地写死 —— 换个读数时另一份会静默过期。',
    );
});

test('script-map/consumers-import-table：两个已知消费方必须 import 常量表（防止把值写成十进制绕过字面量检查）', () => {
    const missing = [];
    for (const { dir, file, table } of CONSUMERS) {
        const text = fs.readFileSync(path.join(dir, file), 'utf8');
        if (!text.includes(`from "${table}"`)) missing.push(`${path.basename(dir)}/${file} 没有 import ${table}`);
    }
    assert.deepEqual(missing, [], missing.join('\n'));
});

test('script-map/positions-export-names-unique：`script-positions.ts` 的导出常量名不重复', () => {
    const text = fs.readFileSync(path.join(LIB_DIR, 'script-positions.ts'), 'utf8');
    const names = [];
    const re = /^\s*export\s+const\s+([A-Za-z_$][\w$]*)\s*=/gm;
    let m;
    while ((m = re.exec(text)) !== null) names.push(m[1]);
    assert.ok(names.length > 0, '`script-positions.ts` 里一个 `export const` 都没解析到（形态变了？）');
    const seen = new Set();
    const dup = [];
    for (const n of names) {
        if (seen.has(n)) dup.push(n);
        seen.add(n);
    }
    assert.deepEqual(dup, [], '`script-positions.ts` 里重复导出了：' + dup.join(', '));
});
