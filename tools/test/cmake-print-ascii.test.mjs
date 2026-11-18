/** @env pure @kind gate @why CMake 会打印的字面量（COMMENT / message / option·set(CACHE) 帮助串）里出现非 ASCII ⇒ ninja 直出的 UTF-8 被 936 终端、管道、重定向日志按本地码页解码成全角乱码，而构建本身照绿 */
/**
 * tools/test/cmake-print-ascii.test.mjs —— **CMake 打印面恒 ASCII** 的守卫
 *
 * ## 红了意味着什么
 * `apps/emulator-lift`（以及任何 CMakeLists / *.cmake）里**会被打印**的引号实参含非 ASCII 字节。
 * 后果不是"字体不好看"，是**日志不可读**：
 *   - `add_custom_command(... COMMENT "…")` 被 CMake 原样写进 `build.ninja` 的 `DESC =`，
 *     ninja 在该命令**开始前**把 UTF-8 原字节直出终端 ⇒ 936 码页下就是 `锛坈lang -c;-O2…` 那种乱码，
 *     而且它同时落进 `> build.log` / CI / `tools/mutate-check.mjs` 的 `logFd`；
 *   - `message(...)` 在 configure 期打印；改了 CMake 之后的第一次 `cmake --build` 会走 `RERUN_CMAKE`
 *     把整段 configure 输出**回放进构建日志**（所以它也是构建日志面）；
 *   - `option()` / `set(... CACHE ...)` 的帮助串只进 `CMakeCache.txt` / `cmake -L`。
 * ⇒ 口径（与 `tests/lift_gen_ascii_output.cpp` 同一条，2026-11-19 父级裁决）：**生产者字节恒 ASCII**。
 *   ⛔ 不去改消费者（`chcp 65001` / 终端设置）：管道 / 重定向 / CI 的编码不可知，`chcp` 还是全局副作用。
 *
 * ## 判据（4 条，全部机械可判）
 * 1. **扫描集合** = 仓库内所有 `CMakeLists.txt` 与 `*.cmake`，跳过构建产物 / 依赖 / 临时区
 *    （`out` / `out-*` / `vendor` / `node_modules` / `.git` / `.tmp` / `.staging` / `dist` / `corpus` / `vcpkg`）。
 *    ⛔ 不扫 `CMakePresets.json`：它不是 CMake 脚本、也没有打印面（含了只会多一条恒真断言）。
 * 2. **注释剥离必须引号感知**：`#` 在 `"…"` 内、在 `[=[…]=]` 实参内**不算注释**；`#[[ … ]]` / `#[=[ … ]=]`
 *    括号注释整段剥（⚠ 实测本仓 0 处括号注释 —— 解析器仍然实现它，因为"今天没有"不是不变量）。
 *    ⇒ 这是**不能用 `rg` 当守卫**的根因：`set(X "#hash")` 会被按行正则截断，跨行 `message("…")` 按行也判不了。
 * 3. **只断言"会打印的引号实参"**：`message(...)` 的全部引号实参 · `add_custom_command(COMMENT "…")` /
 *    `add_custom_target(COMMENT "…")` / `install(CODE|SCRIPT "…")` 的实参 · `option()` 的帮助串 /
 *    `set(... CACHE <TYPE> "…")` 的帮助串。`COMMAND` 里的实参不断言（它们是路径 / flag）。
 * 4. **判据是字节**：`Buffer.from(s,'utf8').every(b => b < 0x80)`。⛔ 不写成"不含中文"：
 *    `（` `）` `⇒` `⛔` `→` 都不是汉字却一样乱码（普查里 14 个非汉字码点就是反例集）。
 *
 * ## 它怎么证明自己不是恒真
 * 同一文件内用**内联字符串**造正 / 负样本：正样本（`message("中文")` + `COMMENT "x（y"`）必须命中 2 处，
 * 负样本（行注释 / 尾注释 / 块注释 / 字符串里的 `#` / 纯 ASCII message）必须 0 处。
 * ⛔ 不断言"当前 57 条"这类数字（AGENTS.md §9：test 不记当前取值）—— 只断言"扫到了东西"与样本间的相对关系。
 *
 * ## 豁免（整文件，带理由；每条都必须真命中至少一个文件，否则报红 ⇒ 豁免表不许变成藏污点）
 * `apps/emulator-cxx/tests/*.cmake` 与 `apps/emulator-lift/tests/cmake/*.cmake` 是**手工 / ctest `-P` 脚本**：
 * 它们的输出进 `LastTest.log`，不进构建日志，也不在本轮裁决的修改范围（父级 2026-11-19 裁决 #2/#3）。
 */
import fs from 'node:fs';
import path from 'node:path';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

/** 构建产物 / 依赖 / 临时区（目录名匹配）。`out` 与 `out-*` 都要跳（§12.3 只允许一棵 `out`，但它不是唯一的存在）。 */
const SKIP_DIR = (name) =>
  name === 'out' ||
  name.startsWith('out-') ||
  name === 'node_modules' ||
  name === 'vendor' ||
  name === 'vcpkg' ||
  name === 'dist' ||
  name === 'corpus' ||
  name === '.git' ||
  name === '.tmp' ||
  name === '.staging' ||
  name === '.cache' ||
  name === '.pnpm-store';

/**
 * 整文件豁免：`相对路径前缀 → 理由`。
 * ★ 为什么按前缀按目录：那两处是**一整类**脚本（ctest `-P` / 手工诊断），逐行豁免 70 条只会让表更难审。
 */
export const ALLOW = new Map([
  [
    'apps/emulator-cxx/tests/',
    'ctest `cmake -P` 判定脚本：输出进 LastTest.log，不进构建日志（父级 2026-11-19 裁决 #2：本轮不动）',
  ],
  [
    'apps/emulator-lift/tests/cmake/',
    '手工诊断脚本（`cmake -P lift_aot_split.cmake`）：没接进 add_test / add_custom_command，不进构建日志',
  ],
]);

/* ───────────────────────── 解析器（引号 / 括号注释感知） ───────────────────────── */

/** 从 `text[i] === '"'` 起跳过一个 CMake 引号实参，返回闭引号的**下标**（未闭合 ⇒ 末尾）。 */
function skipString(text, i) {
  i++; // 开引号
  while (i < text.length) {
    const c = text[i];
    if (c === '\\') {
      i += 2;
      continue;
    }
    if (c === '"') return i;
    i++;
  }
  return text.length;
}

/**
 * 抹掉**注释**（不抹字符串 / 括号实参），偏移与换行**逐一保留** ⇒ 行号可复算。
 * 处理三种：`#` 行注释（到行尾）· `#[[ … ]]` / `#[=[ … ]=]` 括号注释 · `[=[ … ]=]` / `[[ … ]]` 括号**实参**
 * （实参不是注释，但它内部的 `#` 也不是注释 ⇒ 整段跳过）。
 */
export function stripCmakeComments(text) {
  const out = text.split('');
  let i = 0;
  const blank = (from, to) => {
    for (let k = from; k < to && k < out.length; k++) if (out[k] !== '\n') out[k] = ' ';
  };
  while (i < text.length) {
    const c = text[i];
    if (c === '"') {
      i = skipString(text, i) + 1;
      continue;
    }
    if (c === '[') {
      const b = /^\[(=*)\[/.exec(text.slice(i));
      if (b) {
        const close = ']' + b[1] + ']';
        const e = text.indexOf(close, i + b[0].length);
        i = e < 0 ? text.length : e + close.length;
        continue;
      }
    }
    if (c === '#' && text[i + 1] === '[') {
      // 括号注释 `#[[ … ]]` / `#[=[ … ]=]`
      const b = /^#\[(=*)\[/.exec(text.slice(i));
      if (b) {
        const close = ']' + b[1] + ']';
        const e = text.indexOf(close, i + b[0].length);
        const end = e < 0 ? text.length : e + close.length;
        blank(i, end);
        i = end;
        continue;
      }
    }
    if (c === '#') {
      const nl = text.indexOf('\n', i);
      const end = nl < 0 ? text.length : nl;
      blank(i, end);
      i = end;
      continue;
    }
    i++;
  }
  return out.join('');
}

/** 从 `(` 起找配对的 `)`，跳过字符串 / 括号实参。返回 `)` 的下标（未闭合 ⇒ 末尾）。 */
function matchParen(text, open) {
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (c === '"') {
      i = skipString(text, i);
      continue;
    }
    if (c === '[') {
      const b = /^\[(=*)\[/.exec(text.slice(i));
      if (b) {
        const close = ']' + b[1] + ']';
        const e = text.indexOf(close, i + b[0].length);
        i = e < 0 ? text.length : e + close.length - 1;
        continue;
      }
    }
    if (c === '(') depth++;
    else if (c === ')') {
      depth--;
      if (depth === 0) return i;
    }
  }
  return text.length;
}

/** 行号（1-based）：`pos` 之前有多少个 `\n`。 */
const lineOf = (text, pos) => text.slice(0, pos).split('\n').length;

/**
 * 取一个调用体（`( … )` 之间）里的**顶层实参**：`{ value, pos, quoted }`。
 * 只用于 `option()` / `set(... CACHE ...)` 的定位（`COMMENT` / `CODE` 走 `keywordArg`）。
 */
function topLevelArgs(text, from, to) {
  const args = [];
  let i = from;
  while (i < to) {
    const c = text[i];
    if (c === ' ' || c === '\t' || c === '\r' || c === '\n') {
      i++;
      continue;
    }
    if (c === '"') {
      const end = skipString(text, i);
      args.push({ value: text.slice(i + 1, end), pos: i, quoted: true });
      i = end + 1;
      continue;
    }
    if (c === '[') {
      const b = /^\[(=*)\[/.exec(text.slice(i));
      if (b) {
        const close = ']' + b[1] + ']';
        const e = text.indexOf(close, i + b[0].length);
        const end = e < 0 ? to : e + close.length;
        args.push({ value: text.slice(i, end), pos: i, quoted: false });
        i = end;
        continue;
      }
    }
    // 裸词（可能含 `${…}` / `$<…>`）：吃到空白 / `)` 为止，`${` / `$<` 里允许括号
    let j = i;
    let depth = 0;
    while (j < to) {
      const d = text[j];
      if (d === '(') depth++;
      else if (d === ')') {
        if (depth === 0) break;
        depth--;
      } else if (d === ' ' || d === '\t' || d === '\r' || d === '\n') {
        if (depth === 0) break;
      } else if (d === '"') {
        j = skipString(text, j);
      }
      j++;
    }
    args.push({ value: text.slice(i, j), pos: i, quoted: false });
    i = j;
  }
  return args;
}

/**
 * 在调用体里找**关键字 + 紧随其后的引号实参**（`COMMENT "…"` / `CODE "…"` / `SCRIPT "…"`）。
 * 关键字大小写不敏感（CMake 的实参关键字本身大小写不敏感），但只认**顶层**（不在字符串 / 括号实参里）。
 */
function keywordArg(text, from, to, keywords) {
  const found = [];
  let i = from;
  while (i < to) {
    const c = text[i];
    if (c === '"') {
      i = skipString(text, i) + 1;
      continue;
    }
    if (c === '[') {
      const b = /^\[(=*)\[/.exec(text.slice(i));
      if (b) {
        const close = ']' + b[1] + ']';
        const e = text.indexOf(close, i + b[0].length);
        i = e < 0 ? to : e + close.length;
        continue;
      }
    }
    const word = /^[A-Za-z_][A-Za-z0-9_]*/.exec(text.slice(i, to));
    if (word) {
      const w = word[0];
      if (keywords.includes(w.toUpperCase())) {
        let j = i + w.length;
        while (j < to && /[ \t\r\n]/.test(text[j])) j++;
        if (text[j] === '"') {
          const end = skipString(text, j);
          found.push({ value: text.slice(j + 1, end), pos: j });
          i = end + 1;
          continue;
        }
      }
      i += w.length;
      continue;
    }
    i++;
  }
  return found;
}

/** 调用种类 → 报告优先级（影响面：构建日志必进 > configure 日志 > 只进 CMakeCache）。 */
const KIND_RANK = new Map([
  ['add_custom_command(COMMENT)', 0],
  ['add_custom_target(COMMENT)', 0],
  ['install(CODE|SCRIPT)', 0],
  ['message', 1],
  ['option(help)', 2],
  ['set(CACHE help)', 2],
]);

/** 每字节 < 0x80 才算 ASCII（⛔ 不按"有没有汉字"判）。 */
export const isAsciiBytes = (s) => Buffer.from(s, 'utf8').every((b) => b < 0x80);

/**
 * 找出**会被打印的引号实参**。
 * @param {string} text 源文本（未剥注释 —— 本函数自己先剥，偏移不变）
 * @param {string} file 报告用的文件名
 * @returns {{file:string,line:number,kind:string,text:string,pos:number}[]} 全部打印站点（含 ASCII 的）
 */
export function printSites(text, file = '<memory>') {
  const src = stripCmakeComments(text);
  const sites = [];
  const push = (kind, value, pos) => {
    if (!value) return;
    sites.push({ file, line: lineOf(src, pos), kind, text: value, pos });
  };

  // 逐个命令名找调用（`\b` 防 `xmessage(` / `set_property(` 之类）
  const CALL = /\b(message|add_custom_command|add_custom_target|install|option|set)\s*\(/g;
  let m;
  while ((m = CALL.exec(src)) !== null) {
    const open = m.index + m[0].length - 1;
    const close = matchParen(src, open);
    const bodyFrom = open + 1;
    const bodyTo = close;
    switch (m[1]) {
      case 'message': {
        for (const a of topLevelArgs(src, bodyFrom, bodyTo)) if (a.quoted) push('message', a.value, a.pos);
        break;
      }
      case 'add_custom_command':
      case 'add_custom_target': {
        for (const a of keywordArg(src, bodyFrom, bodyTo, ['COMMENT']))
          push(`${m[1]}(COMMENT)`, a.value, a.pos);
        break;
      }
      case 'install': {
        for (const a of keywordArg(src, bodyFrom, bodyTo, ['CODE', 'SCRIPT']))
          push('install(CODE|SCRIPT)', a.value, a.pos);
        break;
      }
      case 'option': {
        const args = topLevelArgs(src, bodyFrom, bodyTo);
        // `option(<name> "<help>" [<default>])`：帮助串 = 第 2 个实参（若它被引号包着）
        const help = args[1];
        if (help && help.quoted) push('option(help)', help.value, help.pos);
        break;
      }
      case 'set': {
        const args = topLevelArgs(src, bodyFrom, bodyTo);
        const ci = args.findIndex((a) => !a.quoted && a.value.toUpperCase() === 'CACHE');
        // `set(<var> <value> CACHE <TYPE> "<help>" [FORCE])`
        const help = ci >= 0 ? args[ci + 2] : undefined;
        if (help && help.quoted) push('set(CACHE help)', help.value, help.pos);
        break;
      }
      default:
        break;
    }
    CALL.lastIndex = close + 1;
  }
  return sites;
}

/** 递归收集仓库里的 CMake 脚本（跳过 SKIP_DIR）。 */
export function cmakeFiles(root = REPO_ROOT) {
  const out = [];
  const walk = (dir) => {
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) {
        if (!SKIP_DIR(e.name)) walk(p);
      } else if (e.isFile() && (e.name === 'CMakeLists.txt' || e.name.endsWith('.cmake'))) {
        out.push(p);
      }
    }
  };
  walk(root);
  return out.sort();
}

/** 文件是否整文件豁免（前缀匹配）；豁免表里没人命中的前缀由守卫自己报红。 */
export const allowReason = (rel) => {
  for (const [prefix, why] of ALLOW) if (rel.startsWith(prefix)) return why;
  return null;
};

/** 扫全仓，返回 `{scanned, exempt, sites, hits}`（`hits` = 非 ASCII 的那些）。 */
export function scanRepo(root = REPO_ROOT) {
  const files = cmakeFiles(root);
  const sites = [];
  const exempt = [];
  for (const abs of files) {
    const rel = path.relative(root, abs).replace(/\\/g, '/');
    if (allowReason(rel)) {
      exempt.push(rel);
      continue;
    }
    sites.push(...printSites(fs.readFileSync(abs, 'utf8'), rel));
  }
  const hits = sites.filter((s) => !isAsciiBytes(s.text));
  // 排序 = 影响面（COMMENT 必进构建日志 → message → CMakeCache 帮助串），同类里**乱码字节多**的排前面。
  //   ★ 只影响报告顺序，不影响判据；让第一行命中先指到"最该先修的那条"。
  const bytes = (s) => Buffer.byteLength(s.text, 'utf8') - Buffer.from(s.text, 'utf8').filter((b) => b < 0x80).length;
  hits.sort(
    (a, b) =>
      KIND_RANK.get(a.kind) - KIND_RANK.get(b.kind) ||
      bytes(b) - bytes(a) ||
      a.file.localeCompare(b.file) ||
      a.line - b.line,
  );
  return { scanned: files.length, exempt, sites, hits };
}

/* ───────────────────────── 自测：正 / 负样本 ───────────────────────── */

test('正样本：message 与 COMMENT 里的非 ASCII 被抓到（证明守卫不是恒真）', () => {
  const src = ['message("中文")', 'add_custom_command(OUTPUT x COMMENT "x（y")'].join('\n');
  const sites = printSites(src, 'pos.cmake');
  assert.equal(sites.length, 2, '两个引号实参都该被取到');
  assert.deepEqual(
    sites.map((s) => s.kind),
    ['message', 'add_custom_command(COMMENT)'],
  );
  assert.deepEqual(
    sites.map((s) => isAsciiBytes(s.text)),
    [false, false],
    '两条都必须判成非 ASCII（`（` 不是汉字，但同样是 3 字节 ⇒ 必须命中）',
  );
});

test('负样本：行注释 / 尾注释 / 块注释 / 纯 ASCII 实参都不算打印面', () => {
  const src = [
    '# 中文行注释 message("中文")',
    'message("ascii only") # 中文尾注释',
    '#[[ 中文块注释 message("中文") ]]',
    '#[=[ 中文块注释带 = 号 ]=]',
    'set(A "x")',
  ].join('\n');
  const sites = printSites(src, 'neg.cmake');
  assert.deepEqual(
    sites.map((s) => s.text),
    ['ascii only'],
    '只有那一条 ASCII 的 message 该被取到；注释里的 `message(` 一个字都不该进来',
  );
  assert.equal(sites.filter((s) => !isAsciiBytes(s.text)).length, 0);
});

test('引号感知：字符串里的 `#` 不截断实参、括号实参里的 `#` 也不算注释', () => {
  const src = [
    'message("hash # 中文")',
    'set(B [=[ # 不是注释 ]=])',
    'message("after bracket")',
  ].join('\n');
  const sites = printSites(src, 'quote.cmake');
  assert.deepEqual(
    sites.map((s) => s.text),
    ['hash # 中文', 'after bracket'],
    '`#` 在字符串 / 括号实参里 = 普通字符 ⇒ 后面的代码仍然要被扫到',
  );
  assert.equal(isAsciiBytes(sites[0].text), false);
  assert.equal(isAsciiBytes(sites[1].text), true);
});

test('多行 message 与 COMMENT 的每个实参都单独判（不是按行判）', () => {
  const src = [
    'message(FATAL_ERROR',
    '    "line one ascii"',
    '    "line two 中文")',
    'add_custom_command(OUTPUT y',
    '    COMMENT "ok ascii")',
  ].join('\n');
  const sites = printSites(src, 'multiline.cmake');
  assert.equal(sites.length, 3);
  assert.deepEqual(
    sites.map((s) => isAsciiBytes(s.text)),
    [true, false, true],
  );
  assert.equal(sites[1].line, 3, '行号要指到实参自己那一行，不是命令名那一行');
});

test('option / set(CACHE) 的帮助串在扫描面内（只进 CMakeCache 的那一类也要 ASCII）', () => {
  const src = [
    'option(FOO "帮助串" OFF)',
    'set(BAR 1 CACHE STRING "cache 帮助")',
    'set(BAZ 2 CACHE PATH "ascii help")',
    'set(NOPE 3)',
  ].join('\n');
  const sites = printSites(src, 'cache.cmake');
  assert.deepEqual(
    sites.map((s) => s.kind),
    ['option(help)', 'set(CACHE help)', 'set(CACHE help)'],
  );
  assert.deepEqual(
    sites.map((s) => isAsciiBytes(s.text)),
    [false, false, true],
  );
});

/* ───────────────────────── 真仓库：门禁 ───────────────────────── */

const REAL = scanRepo();

test('扫描面不为空（防空转：解析器没坏、两棵产品 CMakeLists 都在面里）', () => {
  assert.ok(REAL.scanned > 0, '一个 CMake 脚本都没扫到 ⇒ 后面那条断言会恒真');
  const files = new Set(REAL.sites.map((s) => s.file));
  for (const need of ['apps/emulator-lift/CMakeLists.txt', 'apps/emulator-cxx/CMakeLists.txt']) {
    assert.ok(files.has(need), `${need} 的打印站点没被扫到 ⇒ 判据没作用在它身上`);
  }
  // ★ 打印站点本身**必须**存在（ASCII 的那些）：这条不随"修完变绿"而失效。
  assert.ok(REAL.sites.length > 0, '连一个打印站点都没有 ⇒ 守卫是空转的');
});

test('豁免表不许变成藏污点：每个前缀都真命中至少一个文件', () => {
  for (const [prefix, why] of ALLOW) {
    assert.ok(why && why.trim().length > 0, `豁免 ${prefix} 没写理由`);
    assert.ok(
      REAL.exempt.some((f) => f.startsWith(prefix)),
      `豁免 ${prefix} 一个文件也没命中 ⇒ 文件搬走 / 改名了，豁免该删`,
    );
  }
});

test('CMake 会打印的字面量逐字节 < 0x80（936 / UTF-8 / 管道 三处都可读）', () => {
  const detail = REAL.hits
    .map(
      (h) =>
        `  ${h.file}:${h.line}  ${h.kind}\n      ${JSON.stringify(h.text)}\n` +
        `      non-ASCII bytes: ${[...Buffer.from(h.text, 'utf8')].filter((b) => b >= 0x80).map((b) => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')}`,
    )
    .join('\n');
  assert.equal(
    REAL.hits.length,
    0,
    `⛔ ${REAL.hits.length} 处 CMake 会打印的字面量含非 ASCII ⇒ 936 终端 / 管道 / 重定向日志全部乱码。\n` +
      `怎么修：把中文括注换成 ASCII（保住信息量：谁 / 干什么 / 为什么 / 数字），⛔ 不改消费者（chcp）。\n` +
      `⛔ 注释（#）里的中文不在判据内，一个字都不用动。\n\n${detail}\n`,
  );
});
