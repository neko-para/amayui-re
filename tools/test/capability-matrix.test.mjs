/** @env pure @kind gate @why control 能力面的三处（hello.capabilities / controlOpName / 启动 readout 的 caps=）与 §4.9 的 9 项漂了 —— 加/删一个 op 只改了一处，或 caps= 又少一项，而没有任何运行期读数会喊 */
/**
 * tools/test/capability-matrix.test.mjs —— **control 能力面同源**的静态守卫（C0）
 *
 * ## 红了意味着什么
 * `docs/03-emulator/plugin-runtime.md` §4.9 的合同是"control 的 9 项不变"：
 * `pause / resume / sample / screenshot / move / click / global-get / global-set / quit`，
 * 顺序 = `hello.capabilities` 的数组顺序 = `ControlOp` 的值序。真源 = `src/runtime/capability.h`。
 *
 * 本轮（C0）之前三处**各有一份**：
 *   - `hello.capabilities`（`control_ws.cpp` 的 `helloJson`）—— 9 项，有 CTest 守着；
 *   - `controlOpName`（同文件）—— 手写 9 个 `case`；
 *   - `caps=` 启动 readout（`aot_main.cpp`）—— **6 项**（漏 `move` / `global-get` / `global-set`），
 *     且没有任何守卫断言它 ⇒ 漂了很久没人发现（`plugin-runtime.md` §1.3 G7 / §1.4）。
 * ⇒ 这条守卫守的不是"今天有 9 项"这个读数，而是**三处只有一个来源**：
 *   表里加/删一项 ⇒ 这里红；某处又抄一份字面量清单 ⇒ 这里红；`wait` 被塞进服务端 ⇒ 这里红。
 *
 * ## 判据（5 组，全部读源码文本 / 客户端表；⛔ 不构建、⛔ 不跑 CTest）
 * A. 表的**名字序列** = 合同那 9 项且**精确有序**；枚举值序 = 表的行序；表长由 `kControlCapabilityCount`
 *    绑定（少一项是**编译期**错误，不靠人眼数）。
 * B. `helloJson` 从 `kControlCapabilities` 生成、`controlOpName` 只调 `controlCapabilityName`
 *    （⛔ 不再有 `json::array({"pause", …})` 那种字面量清单，⛔ 不再有手写 `case ControlOp::`）；
 *    两个枚举的值序有 `static_assert` 绑着。
 * C. `caps=` 用**同一张表**的 CSV：`controlCapabilitiesCsv()` 生成、printf 用 `%s`。
 * D. `control.h:13-21` 的注释**指向** `runtime/capability.h`（⛔ 不再手抄清单）。
 * E. `Wait` 不是服务端 op：`ControlOp` 里没有它，它只在 `tools/lib/emu-ctl.mjs` 的客户端糖里。
 *
 * ## 为什么不断言 `--help`
 * §5 C0 的判据是"`--help` 文本**不改**"（它早就是 9 项，`aot_main.cpp` 的 `能力面（本轮）= …`）。
 * 把中文排版拷进断言 ⇒ 改一个标点就红，而它并不表示任何漂移（`AGENTS.md` §10：不要断言易变形状）。
 *
 * ## 它怎么证明自己不是恒真
 * `tools/mutate-check.mjs` 两条：① 删能力表的一项 ⇒ 本文件 A 组红（`caps=` 是唯一没有 CTest 的面）；
 * ② 让 `hello` 的循环少枚举一项 ⇒ `ctest:lift-control-endpoint`（`tests/control_endpoint.cpp:645-647`
 * 那条逐字 `hello` 断言）红。两条各点名一类机制，⛔ 不给每个能力配一条。
 */
import fs from 'node:fs';
import path from 'node:path';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

const HEADER = 'apps/emulator-lift/src/runtime/capability.h';
const WS = 'apps/emulator-lift/src/runtime/control_ws.cpp';
const MAIN = 'apps/emulator-lift/src/aot/aot_main.cpp';
const CONTROL_H = 'apps/emulator-lift/src/runtime/control.h';
const EMU_CTL = 'tools/lib/emu-ctl.mjs';

/** 合同（`plugin-runtime.md` §4.9）：**顺序即合同**，与 `ControlOp` 值序逐项对齐。 */
const CONTRACT = ['pause', 'resume', 'sample', 'screenshot', 'move', 'click', 'global-get', 'global-set', 'quit'];

/** 枚举项序（`None` = 哨兵，不是能力；其后 = 表行序）。 */
const CAP_ENUM = ['None', 'Pause', 'Resume', 'Sample', 'Screenshot', 'Move', 'Click', 'GlobalGet', 'GlobalSet', 'Quit'];

const files = new Map();
/** 读仓库内文本（缓存；相对路径 `/` 分隔）。 */
function src(rel) {
  if (!files.has(rel)) files.set(rel, fs.readFileSync(path.join(ROOT, rel), 'utf8'));
  return files.get(rel);
}

/**
 * 取 `signature` 之后**第一对配对的 `{}`** 的原文（含花括号）。
 * 跳过字符串字面量、行注释、块注释 —— 这些里面出现 `{`/`}` 不该改变深度。
 * ⚠ 字符字面量不特判：这几段里没有 `'{'`/`'}'` 这种写法（真有的话会**假红**而不是假绿，可接受）。
 * @returns {string|null} 找不到 signature / 花括号不配对 ⇒ null
 */
function braceBody(text, signature) {
  const at = text.indexOf(signature);
  if (at < 0) return null;
  let i = text.indexOf('{', at + signature.length);
  if (i < 0) return null;
  let depth = 0;
  for (let j = i; j < text.length; j++) {
    const c = text[j];
    if (c === '"') {
      j++;
      while (j < text.length && text[j] !== '"') {
        if (text[j] === '\\') j++;
        j++;
      }
      continue;
    }
    if (c === '/' && text[j + 1] === '/') {
      const nl = text.indexOf('\n', j);
      j = nl < 0 ? text.length : nl;
      continue;
    }
    if (c === '/' && text[j + 1] === '*') {
      const end = text.indexOf('*/', j + 2);
      j = end < 0 ? text.length : end + 1;
      continue;
    }
    if (c === '{') depth++;
    else if (c === '}') {
      depth--;
      if (depth === 0) return text.slice(i, j + 1);
    }
  }
  return null;
}

/** 从枚举体里按**声明顺序**取项名（`X` / `X = 0` 两种写法都认）。 */
function enumNames(body) {
  return [...body.matchAll(/([A-Z][A-Za-z0-9]*)\s*(?:=\s*[^,}]+)?\s*,/g)].map((m) => m[1]);
}

/** 取从 `(` 起到配对 `)` 的原文（`static_assert(...)` 的实参表用；本文件的实参里没有嵌套字符串）。 */
function parenBody(text, openParen) {
  let depth = 0;
  for (let i = openParen; i < text.length; i++) {
    if (text[i] === '(') depth++;
    else if (text[i] === ')') {
      depth--;
      if (depth === 0) return text.slice(openParen, i + 1);
    }
  }
  return '';
}

/* ───────────────────────── A. 表 = 9 项 · 顺序 = 合同 ───────────────────────── */

test('A) 能力表的名字序列 = 合同那 9 项且精确有序（`wait` 不在里面）', () => {
  const header = src(HEADER);
  const table = braceBody(header, 'kControlCapabilities[]');
  assert.ok(table, `${HEADER}: 找不到 \`kControlCapabilities[kControlCapabilityCount]\` 的初始化体`);

  const rows = [...table.matchAll(/\{\s*ControlCapability::([A-Za-z0-9]+)\s*,\s*"([^"]+)"\s*\}/g)];
  const names = rows.map((m) => m[2]);
  const values = rows.map((m) => m[1]);

  assert.deepEqual(
    names,
    CONTRACT,
    `能力表的名字序列 ≠ 合同（§4.9）。实际 ${JSON.stringify(names)}；期望 ${JSON.stringify(CONTRACT)}。\n` +
      '⇒ 加/删一个 control op 必须同时改这张表、`plugin-runtime.md` §4.9 与 `.d.ts` 的映射表（⛔ 三处各抄一份）。',
  );
  assert.deepEqual(
    values,
    CAP_ENUM.slice(1),
    `表行的枚举值序列 ≠ \`ControlCapability\` 的声明序（${JSON.stringify(CAP_ENUM.slice(1))}）⇒ ` +
      '顺序与值序漂了：`hello.capabilities` 的数组顺序 = `ControlOp` 值序，两处必须逐项对齐。',
  );
  assert.ok(!names.includes('wait'), '`wait` 是 emu-ctl 的客户端糖（= `resume --for-ms`），⛔ 不是服务端 op');

  // 表长由枚举项数绑定 ⇒ 少一项 / 多一项都是**编译期**错误（这里守的是那条绑定还在）
  const binds = [...header.matchAll(/static_assert\s*\(/g)]
    .map((mm) => parenBody(header, mm.index + mm[0].length - 1))
    .find((a) => a.includes('kControlCapabilities') && a.includes('kControlCapabilityCount'));
  assert.ok(binds,
    `${HEADER}: 表行数必须有一条 \`static_assert\` 绑到 \`kControlCapabilityCount\`（否则"少一项"只能靠人眼数：` +
      'C++ 会把缺的那一行值初始化成 `{None, nullptr}`，⛔ 不报错）');
  assert.match(header, /kControlCapabilityCount\s*=\s*static_cast<std::size_t>\(\s*ControlCapability::Quit\s*\)/,
    `${HEADER}: \`kControlCapabilityCount\` 必须从 \`ControlCapability::Quit\` 导出（值序 = 项数）`);
});

test('A2) `ControlCapability` 的声明序 = 表行序（None 之后逐项对齐）', () => {
  const body = braceBody(src(HEADER), 'enum class ControlCapability');
  assert.ok(body, `${HEADER}: 找不到 \`enum class ControlCapability\` 的体`);
  assert.deepEqual(enumNames(body), CAP_ENUM,
    '`ControlCapability` 的项序/项名变了 ⇒ 需要同步 §4.9 的 9 项合同（⛔ 不要靠"表在就行"）');
});

/* ───────────────────────── B. hello / opName 同源 ───────────────────────── */

test('B) hello.capabilities 从表生成（⛔ 不再有字面量清单）', () => {
  const ws = src(WS);
  const hello = braceBody(ws, 'std::string Control::Impl::helloJson()');
  assert.ok(hello, `${WS}: 找不到 \`Control::Impl::helloJson()\` 的体`);
  assert.match(
    hello,
    /for\s*\([^)]*:\s*plugin::kControlCapabilities\s*\)/,
    '`hello.capabilities` 必须遍历 `runtime/capability.h` 的 `kControlCapabilities` 生成（C0 的同源要求）',
  );
  assert.match(hello, /push_back\s*\(\s*row\.name\s*\)/, '`hello.capabilities` 的每一项必须取表中的 `row.name`');
  assert.doesNotMatch(
    hello,
    /json::array\s*\(\s*\{/,
    '`helloJson` 又出现了 `json::array({ … })` 这种**字面量**能力数组 —— 那正是 `caps=` 长期漂移的形态；' +
      '能力名只许从 `kControlCapabilities` 取',
  );
});

test('B2) controlOpName 只查表（⛔ 不再手写 switch / 字面量）', () => {
  const opName = braceBody(src(WS), 'const char* controlOpName(ControlOp op)');
  assert.ok(opName, `${WS}: 找不到 \`controlOpName(ControlOp op)\` 的体`);
  assert.match(opName, /controlCapabilityName\s*\(/, '`controlOpName` 必须查 `controlCapabilityName`（名字只有一份）');
  assert.doesNotMatch(opName, /\bcase\s+ControlOp::/, '`controlOpName` 不许再手写 switch（加了 op 会漏一处）');
  assert.doesNotMatch(
    opName,
    /return\s+"(pause|resume|sample|screenshot|move|click|global-get|global-set|quit)"/,
    '`controlOpName` 里不许出现能力名字面量',
  );
});

test('B3) 两个枚举的值序有 static_assert 绑着（leaf 头的唯一机械判据）', () => {
  const ws = src(WS);
  const asserts = [...ws.matchAll(/static_assert\s*\(/g)].map((m) => parenBody(ws, m.index + m[0].length - 1));
  const hit = asserts.find((a) => a.includes('ControlOp::Quit') && a.includes('ControlCapability::Quit'));
  assert.ok(
    hit,
    `${WS}: 找不到把 \`ControlOp::Quit\` 与 \`ControlCapability::Quit\` 绑在一起的 \`static_assert\` ⇒ ` +
      '两个枚举可以各自漂而没有任何编译期症状（`capability.h` 是 leaf 头，看不见 `ControlOp`）',
  );
  assert.match(ws, /#include\s+"runtime\/capability\.h"/, `${WS}: 必须 include 能力表（否则上面的断言对象不存在）`);
});

/* ───────────────────────── C. caps= 与 hello 同源 ───────────────────────── */

test('C) `caps=` 走同一张表的 CSV（8 项漂成 6 项那次就是这么来的）', () => {
  const header = src(HEADER);
  const csv = braceBody(header, 'inline std::string controlCapabilitiesCsv()');
  assert.ok(csv, `${HEADER}: 找不到 \`controlCapabilitiesCsv()\` 的体`);
  assert.match(csv, /kControlCapabilities/, 'CSV 必须来自同一张表（⛔ 第二份清单）');
  assert.match(csv, /row\.name/, 'CSV 的每一项必须取表里的 `name`（顺序因此由表决定）');

  const main = src(MAIN);
  assert.match(main, /#include\s+"runtime\/capability\.h"/, `${MAIN}: 必须 include 能力表`);
  assert.match(main, /controlCapabilitiesCsv\s*\(\s*\)/,
    `${MAIN}: \`caps=\` 必须由 \`lift::plugin::controlCapabilitiesCsv()\` 生成（⛔ 第二份清单）`);
  assert.match(main, /caps=%s/, '`caps=` readout 必须把 CSV 当 `%s` 实参打（⛔ 不许再写字面量清单）');
  assert.doesNotMatch(main, /caps=pause/, '`caps=` 不许再是字面量清单（那正是 6 项漂移的形态）');
});

/* ───────────────────────── D. control.h 的注释 ───────────────────────── */

test('D) control.h 的能力面注释指向表（真源只有一份）', () => {
  const controlH = src(CONTROL_H);
  const head = controlH.split('#pragma once')[0];
  assert.match(head, /runtime\/capability\.h/, '`control.h` 头注释必须指向 `runtime/capability.h`（真源只有一份）');
});

/* ───────────────────────── E. Wait 只在客户端 ───────────────────────── */

test('E) `Wait` 不是服务端 op：`ControlOp` 里没有它，它只在 emu-ctl 的客户端糖里', () => {
  const opBody = braceBody(src(CONTROL_H), 'enum class ControlOp');
  assert.ok(opBody, `${CONTROL_H}: 找不到 \`enum class ControlOp\` 的体`);
  const ops = enumNames(opBody);
  assert.deepEqual(ops, CAP_ENUM,
    `\`ControlOp\` 的项序必须与能力表逐项对齐（${JSON.stringify(CAP_ENUM)}）；实际 ${JSON.stringify(ops)}`);
  assert.ok(!ops.includes('Wait'), '`wait` 是 `emu-ctl` 的客户端糖（= `resume --for-ms`），⛔ 不是服务端 ControlOp');

  const emu = src(EMU_CTL);
  assert.match(emu, /name:\s*'wait'/, '`wait` 必须以**客户端命令**的身份留在 emu-ctl（糖不能被顺手删掉）');
});
