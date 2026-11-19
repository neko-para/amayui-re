/** @env pure @kind gate @why 插件运行时的核心（C1b 的新文件）里长出 plan 状态机 —— 用户点名的"emulator 内 plan VM"成本又回来了；或者 `Context` 又拿到了 `TriggerRunner`（回调能递归 pump / 绕过"返回值替换集合"） */
/**
 * `apps/emulator-lift` 插件运行时**核心**的源码形态守卫（纯文本，不构建、不跑 exe）。
 *
 * 引用：`REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY` 的 C1b / C1c；合同 `docs/03-emulator/plugin-runtime.md`
 * §6 行 1 / 5；施工图 `.tmp/plugin-runtime/c1-design-review.md` §7.2（本文件就是那一段的落地；
 * C1c 追加 `intent*` / `edit_sink*` / `engine_read.h` 七个文件）。
 *
 * ## 判据（红了意味着什么）
 * ① **白名单逐文件读**（⛔ 不是扫目录）：`src/runtime/` 是**共用**目录（套件 / shim 的邻居），
 *    扫目录会把配方执行器自己的文件也算成"核心" ⇒ 只能显式列。另有一条**地板**：
 *    白名单里每个文件都必须存在且非空 —— 否则改名 / 搬走之后本守卫会**静默变绿**（扫空集合）。
 * ② **核心不许出现 plan 状态机的标识符**：`class InputPlan` / `InputPlan::` /
 *    `input_plan.cpp`（**文件名**，⛔ 不是裸 `input_plan`：核心必须 include `input_point.h`）/
 *    `recipe.h` / `RecipeOutcome` / `StepKind` / `struct Step` / `next_` / `applied_` /
 *    `traceStepConsumed`。
 *    ★ **为什么这些词**：它们逐个都是"配方 VM 的形状"（当前步 / 已施加 / 步骤种类 / 消费 trace）。
 *      C1b 的 trigger 语义只需要"armed 集合 + per-arm 槽"，C1c 的 Intent 语义只需要"两级队列 +
 *      写预算"，两者都**不需要**任何"当前步"概念。
 *    ⛔ **不要禁 `pending`**：`PendingRelease` / `pending_` 是指针待抬起队列（`PointerCore` 的正当
 *      成员，合同 §6 行 1 的 `pending` 是词法不精确）；禁了会把正当实现挡在门外。
 *    ★ **只扫代码**（`stripComments` 剥 `//` 行注释与块注释；字符串 / 字符字面量原样保留）：
 *      注释里**必须**能写出"这一段与配方执行器的 `atPoint` 同形 / 不同"这类说明 —— 把注释也算进去，
 *      "解释判据"本身就会变成违规。C1a 的 `input_point.h` / `pointer_core.*` 就是被旧口径挡在门外的
 *      （8 命中**全在注释**、代码行 0 命中）⇒ 文件集合漏了 3 个 = 守卫缺口。
 *      字符串字面量**不剥**：`"InputPlan::"` 出现在代码里仍是命中（那不是注释，是第二套名字）。
 *      剥注释时**保留换行** ⇒ 报出的行号与源文件一致。
 * ③ **`Context` 不认识 `TriggerRunner`**：`ctx.trigger` 必须是 **`TriggerFactory*`**（施工图 §4.3）。
 *    否则回调里能 `runner.atPoint(...)` 递归、或绕过"返回值完整替换集合"的语义 ——
 *    那是"一个能力两处实现"的入口，而且**没有运行期症状**。
 *
 * ## 为什么现在就要它（⛔ 不要推迟到 C2/C4）
 * C1b 一落地，"核心没长出 plan VM"就是**当天**可被静默破坏的性质（下一个人给 trigger 加个
 * `step`/`next` 字段不会有任何症状）。词表已经足够精确 ⇒ 先钉住。
 *
 * ## 反向自检
 * 判据函数对本文件的假文本与真实文件是**同一份**；用例 ② 往假文本里注入 `next_` 必须被报出来
 * （否则"0 命中"只是个恒真断言），并分别钉住 `stripComments` 的两侧：
 * `// InputPlan::fake()` **不**命中 / `InputPlan::fake();` 命中 / 块注释不命中且行号不漂。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const ROOT = new URL('../../', import.meta.url);

/** ★ 白名单 = C1a/C1b/C1c 落地的核心文件（逐文件读；⛔ 不要改成扫目录）。 */
const CORE_FILES = [
  // ── C1a：`InputPlan : PointerCore` 抽出来的共享实现 —— C1b/C1c 的核心直接继承它，
  //    所以它同样"不许长出配方状态机"（文件集合漏了这 3 个 = 守卫缺口，见头注 ②）。
  'apps/emulator-lift/src/runtime/input_point.h',
  'apps/emulator-lift/src/runtime/pointer_core.h',
  'apps/emulator-lift/src/runtime/pointer_core.cpp',
  'apps/emulator-lift/src/runtime/trigger.h',
  'apps/emulator-lift/src/runtime/trigger_runner.h',
  'apps/emulator-lift/src/runtime/trigger_runner.cpp',
  'apps/emulator-lift/src/runtime/trigger_factory.cpp',
  'apps/emulator-lift/src/runtime/budget.h',
  'apps/emulator-lift/src/runtime/budget.cpp',
  'apps/emulator-lift/src/runtime/context.h',
  // ── C1c：Intent（插件侧 Input）+ EditSink（Edit）+ EngineRead（只读）+ executor（唯一 pump）──
  'apps/emulator-lift/src/runtime/intent.h',
  'apps/emulator-lift/src/runtime/intent_queue.h',
  'apps/emulator-lift/src/runtime/intent_queue.cpp',
  'apps/emulator-lift/src/runtime/intent_executor.h',
  'apps/emulator-lift/src/runtime/intent_executor.cpp',
  'apps/emulator-lift/src/runtime/engine_read.h',
  'apps/emulator-lift/src/runtime/edit_sink.h',
  'apps/emulator-lift/src/runtime/edit_sink.cpp',
  // ── C2a：读点观察者（核心面；`input.h` 四个投递口的第二个接收者）+ 插件适配层 ──
  //   ★ `src/plugin/**` 为什么在这个白名单里：它是 **guest 写 / 输入施加**的**第二入口**面 ——
  //     一旦它自己直接 `input::*` 或 `writeGlobal`（而不是只经 `IntentQueue` / `EditSink`），
  //     就又多了一条施加路径，而且**没有运行期症状**（合同 §6 行 11 / 22）。
  //   ★ 这 7 个文件逐文件列出（⛔ 不扫目录）：`src/plugin/` 将来会长出 C2b/C2c 的东西，
  //     扫目录会把它们一并算成"核心"。
  'apps/emulator-lift/src/runtime/read_point.h',
  'apps/emulator-lift/src/plugin/plugin_host.h',
  'apps/emulator-lift/src/plugin/plugin_host.cpp',
  'apps/emulator-lift/src/plugin/guest_read.h',
  'apps/emulator-lift/src/plugin/guest_read.cpp',
  'apps/emulator-lift/src/plugin/readings.h',
  'apps/emulator-lift/src/plugin/readings.cpp',
];

/** ★ 核心不许出现的词（每条都是"配方 VM 的一个形状"；理由见头注 ②）。 */
const CORE_FORBIDDEN = [
  'class InputPlan',
  'InputPlan::',
  'input_plan.cpp',
  'recipe.h',
  'RecipeOutcome',
  'StepKind',
  'struct Step',
  'next_',
  'applied_',
  'traceStepConsumed',
];

/**
 * ★ **插件适配层专属**禁用词（合同 `plugin-runtime.md` §6 行 11 / 22；C2a 复审 F4）。
 *
 * 判据（红了意味着什么）：`src/plugin/**` 是"guest 写 / 输入施加"的**第二入口**面 ——
 *   · `quickjs.h` / `quickjs_value.h` ⇒ 第二个 QJS TU（§6 行 11 明的编译约束）；
 *   · `input::` ⇒ 绕过 `IntentQueue` 直接施加输入（第二个施加点）；
 *   · `writeGlobal` ⇒ 绕过 `EditSink` 直接写 guest。
 *   这三条**现在都是零命中**（只出现在注释里），但它们正是"下一次改动不会有症状"的那一类。
 *
 * ★ 为什么**不能**并进 `CORE_FORBIDDEN`：`intent_executor.cpp` / `pointer_core.cpp` 合法调用
 *   `input::releaseProgram` / `input::mouseMove`（它们是施加层的**实现**，不是插件）；
 *   `edit_sink.cpp` 里 `writeGlobal` 是 Edit 的唯一实现。⇒ 只对 `src/plugin/**` 生效。
 */
const PLUGIN_FORBIDDEN = ['quickjs.h', 'quickjs_value.h', 'input::', 'writeGlobal'];

/** 白名单里属于插件适配层的那些（按路径过滤，⛔ 不手写第二份清单）。 */
const PLUGIN_FILES = CORE_FILES.filter((rel) => rel.includes('/src/plugin/'));

function read(rel) {
  return fs.readFileSync(new URL(rel, ROOT), 'utf8');
}

/** 判据本体（纯函数；真实文件与注入的假文本**同一份**）。返回 `{line, token, text}` 列表。 */
function forbiddenHits(text, tokens = CORE_FORBIDDEN) {
  const hits = [];
  const lines = stripComments(text).split('\n');
  for (let i = 0; i < lines.length; i += 1) {
    for (const token of tokens) {
      if (lines[i].includes(token)) hits.push({ line: i + 1, token, text: lines[i].trim() });
    }
  }
  return hits;
}

/**
 * 剥 C++ 注释、**保留字符串 / 字符字面量**（`"//"` 里的 `//` 不是注释；`"InputPlan::"` 仍是命中）。
 *
 * ★ 为什么需要它（M2）：判据只该管**代码**。核心的注释必须能写"这一段与配方执行器的 `atPoint`
 *   同形"，否则"解释判据"本身就成了违规 —— C1a 的 3 个文件正是这样被挡在门外的（8 命中全在注释）。
 * ★ 剥块注释时**把换行还回去**：`forbiddenHits` 用 `split('\n')` 报行号，行号漂了定位就没用了。
 * ★ **C++14 数字分隔符不是字符字面量**（`2'000'000ull` / `100'000u`，`budget.h:19/21`）：`'` 的**左右
 *   都是数字**时原样输出、不进字符字面量状态。否则 `100'000u` 会被当成第 3 个 `'`（奇数）⇒ 从那里到
 *   文件尾整段被当成"字符串"⇒ 尾部注释**没被剥**（方向只会假红，但会误导改守卫的人）。
 * ★ 自证：用例 ② 的反自检（`//` 不命中 / 块注释不命中 / 代码命中 / 字符串字面量仍命中 /
 *   字符串里的 `//` 不被当注释 / 跨行块注释后行号不漂 / 数字分隔符之后注释仍被剥）—— 每条钉住一侧。
 */
function stripComments(text) {
  /** C++14 数字分隔符的判据：`'` 的两侧**都是 ASCII 数字**（`2'000'000ull` / `0xFF'FF`）。 */
  const isDigit = (ch) => ch !== undefined && ch >= '0' && ch <= '9';
  let out = '';
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    // ★ 先判数字分隔符（在字符字面量之前）：否则 `100'000u` 的 `'` 会开启一个字符字面量，
    //   吃掉后面整段代码（连注释一起不剥 ⇒ 见头注）。
    if (c === "'" && isDigit(text[i - 1]) && isDigit(text[i + 1])) {
      out += c;
      i += 1;
      continue;
    }
    if (c === '"' || c === "'") {
      const quote = c;
      out += c;
      i += 1;
      while (i < text.length) {
        const d = text[i];
        out += d;
        i += 1;
        if (d === '\\') {
          if (i < text.length) {
            out += text[i];
            i += 1;
          }
          continue;
        }
        if (d === quote) break;
      }
      continue;
    }
    if (c === '/' && text[i + 1] === '/') {
      while (i < text.length && text[i] !== '\n') i += 1;
      continue; // 行尾换行留给下一轮原样带出（⇒ 行号不漂）
    }
    if (c === '/' && text[i + 1] === '*') {
      i += 2;
      while (i < text.length && !(text[i] === '*' && text[i + 1] === '/')) {
        if (text[i] === '\n') out += '\n';
        i += 1;
      }
      i += 2;
      continue;
    }
    out += c;
    i += 1;
  }
  return out;
}

test('① 白名单文件都在、都非空（改名 / 搬走 ⇒ 不许静默扫空集合）', () => {
  const missing = [];
  for (const rel of CORE_FILES) {
    const url = new URL(rel, ROOT);
    if (!fs.existsSync(url)) {
      missing.push(`${rel}（不存在）`);
      continue;
    }
    if (read(rel).trim() === '') missing.push(`${rel}（空文件）`);
  }
  assert.deepEqual(missing, [], `插件运行时核心的白名单文件缺失（本守卫会因此扫空集合而恒绿）：\n  ${missing.join('\n  ')}`);
});

test('② 核心的每个文件里 0 命中 plan 状态机标识符', () => {
  const reported = [];
  for (const rel of CORE_FILES) {
    for (const hit of forbiddenHits(read(rel))) {
      reported.push(`${rel}:${hit.line}: ${hit.token}  ← ${hit.text}`);
    }
  }
  assert.deepEqual(
    reported,
    [],
    '插件运行时核心里出现了配方 VM 的形状（用户点名的"emulator 内 plan VM"成本又回来了）：\n  ' +
      reported.join('\n  '),
  );

  // ★ 反向自检：判据不是恒真 —— 注入一个禁用词必须被点名（且只在那一行）。
  const probe = forbiddenHits('void f() {\n    next_ = 0;\n}\n');
  assert.equal(probe.length, 1, '注入 `next_` 必须恰好被报一条（否则上面的"0 命中"是恒真断言）');
  assert.equal(probe[0].token, 'next_');
  assert.equal(probe[0].line, 2);

  // ★ **只扫代码**的两侧（否则"白名单里 0 命中"可能只是"没扫到"或"连注释一起放过"）：
  assert.equal(
    forbiddenHits('// InputPlan::fake()\n').length,
    0,
    '`//` 行注释里的引用**不许**命中（注释必须能解释"这里与配方执行器不同"）',
  );
  assert.equal(
    forbiddenHits('/* InputPlan::fake() */\n').length,
    0,
    '`/* … */` 块注释同理（口径必须与 `//` 一致，否则只剩一半）',
  );
  assert.equal(
    forbiddenHits('void f() { InputPlan::fake(); }\n').length,
    1,
    '**代码行**里的 `InputPlan::` 必须命中（否则剥注释把判据剥没了）',
  );
  assert.equal(
    forbiddenHits('const char* s = "InputPlan::fake";\n').length,
    1,
    '字符串字面量**不是**注释：`"InputPlan::"` 仍是命中（那代表代码里真有第二套名字）',
  );
  assert.equal(
    forbiddenHits('const char* u = "// next_";\n').length,
    1,
    '字符串里的 `//` 不许被当成注释（否则串里的禁用词会被静默漏掉 ⇒ 判据被剥注释剥没了）',
  );
  const spanned = forbiddenHits('/* 块注释\n   跨了两行 */\nnext_ = 0;\n');
  assert.equal(spanned.length, 1, '跨行块注释之后仍要命中');
  assert.equal(spanned[0].line, 3, '剥块注释必须保留换行 ⇒ 行号与源文件一致（否则定位没意义）');

  // ★ C++14 **数字分隔符**（`budget.h:19/21` 的 `2'000'000ull` / `100'000u`）不是字符字面量：
  //   它后面的 `//` 注释必须照样被剥，它后面的代码行也必须照样被扫（⛔ 不许整段被当成字符串）。
  const digitSep = forbiddenHits("auto v = 100'000u;  // InputPlan::fake()\nnext_ = 0;\n");
  assert.equal(digitSep.length, 1, '数字分隔符之后的 `//` 注释仍要被剥（只该命中第 2 行的 `next_`）');
  assert.equal(digitSep[0].token, 'next_');
  assert.equal(digitSep[0].line, 2, '数字分隔符不许吃掉后续行（否则 budget.h 尾部整段"不是代码也不是注释"）');
  // 反向：真正的字符字面量仍是字面量（`'x'` 之后的代码照扫，`'` 里的词不被当代码）。
  const charLit = forbiddenHits("const char q = 'x';\nnext_ = 0;\n");
  assert.equal(charLit.length, 1, '真字符字面量仍按字面量处理（第 2 行照扫）');
  assert.equal(charLit[0].line, 2);

  // ★ 反"剥过头"（否则"0 命中"可能只是把代码一起剥掉了）：逐文件钉两条骨架不变量 ——
  //   ① 行数不变（⇒ 报出的行号可定位）；② 仍看得到 `#pragma` / `#include`（真代码没被吞）。
  const skeleton = [];
  for (const rel of CORE_FILES) {
    const raw = read(rel);
    const code = stripComments(raw);
    if (code.split('\n').length !== raw.split('\n').length) skeleton.push(`${rel}：剥注释后行数变了`);
    if (!/#(?:pragma|include)/.test(code)) skeleton.push(`${rel}：剥注释后看不到 #pragma / #include（代码被一起剥掉了）`);
  }
  assert.deepEqual(skeleton, [], `剥注释把代码剥掉了（⇒ 本守卫会静默变瞎）：\n  ${skeleton.join('\n  ')}`);
});

test('③ `Context` 只认识工厂、不认识 runner（回调拿不到唯一 pump）', () => {
  const context = read('apps/emulator-lift/src/runtime/context.h');
  assert.ok(
    /TriggerFactory\*\s+trigger\s*=\s*nullptr;/.test(context),
    'context.h：`ctx.trigger` 必须是 `TriggerFactory* trigger = nullptr;`（回调要能造对象、⛔ 不能递归 pump）',
  );
  // ★ 只看**代码行**（`stripComments` 之后）：头注里必须**能写出**这条不变量（"`trigger` 不是 runner"），
  //   把注释也算进去会让"解释判据"本身变成违规。与 ② 共用同一份剥注释实现（⛔ 不许两套口径）。
  const runnerHits = stripComments(context)
    .split('\n')
    .map((text, i) => ({ line: i + 1, text: text.trim() }))
    .filter((h) => h.text.includes('TriggerRunner'));
  assert.deepEqual(
    runnerHits.map((h) => `context.h:${h.line}: ${h.text}`),
    [],
    'context.h 的**代码**里出现了 `TriggerRunner`：回调一旦拿到唯一 pump，就能递归 atPoint / 绕过"返回值替换集合"：\n  ' +
      runnerHits.map((h) => `context.h:${h.line}: ${h.text}`).join('\n  '),
  );
});

test('④ `src/plugin/**` 不 include quickjs、不调 `input::` / `writeGlobal`（§6 行 11 / 22）', () => {
  // ★ 白名单必须**真的**覆盖到插件层：过滤后为空 ⇒ 本用例会"扫空集合"而恒绿。
  assert.ok(PLUGIN_FILES.length > 0, 'PLUGIN_FILES 为空 ⇒ 本守卫会扫空集合而恒绿（白名单被改名/搬走了？）');
  const reported = [];
  for (const rel of PLUGIN_FILES) {
    for (const hit of forbiddenHits(read(rel), PLUGIN_FORBIDDEN)) {
      reported.push(`${rel}:${hit.line}: ${hit.token}  ← ${hit.text}`);
    }
  }
  assert.deepEqual(
    reported,
    [],
    '插件适配层里出现了第二 QJS TU / 第二施加点（合同 §6 行 11 / 22；C2a 复审 F4）：\n  ' +
      reported.join('\n  '),
  );

  // ★ 反向自检（否则"0 命中"可能只是判据写错了）：注入三种词各一条必须被点名，且**只在那一行**；
  //   而**注释**里的同名词不许命中（注释必须能解释"为什么这里不调 input::"）。
  for (const token of PLUGIN_FORBIDDEN) {
    const probe = forbiddenHits(`void f() {\n    ${token} x;\n}\n`, PLUGIN_FORBIDDEN);
    assert.ok(probe.length >= 1, `注入 \`${token}\` 必须被报出来（否则本用例是恒真断言）`);
    assert.equal(probe[0].line, 2, `注入 \`${token}\` 的报点行号必须是 2（剥注释不能漂行）`);
  }
  assert.equal(
    forbiddenHits('// 这里不调 input:: / writeGlobal，也不 include "quickjs.h"\n', PLUGIN_FORBIDDEN).length,
    0,
    '注释里的 `input::` / `writeGlobal` / `quickjs.h` **不许**命中（否则解释判据本身就违规）',
  );
  assert.equal(
    forbiddenHits('const char* s = "input::mouseMove";\n', PLUGIN_FORBIDDEN).length,
    1,
    '字符串字面量**不是**注释：`"input::"` 仍要命中（那代表代码里真有第二个施加点）',
  );
});

test('⑤ `quickjs.h` 的 include 图只有一条：`ui/quickjs_value.h` → `ui/script_quickjs.cpp`（§6 行 11）', () => {
  const SRC = new URL('apps/emulator-lift/src/', ROOT);
  const files = [];
  const walk = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const child = new URL(`${entry.name}${entry.isDirectory() ? '/' : ''}`, dir);
      if (entry.isDirectory()) {
        walk(child);
      } else if (/\.(h|cpp)$/.test(entry.name)) {
        files.push(child);
      }
    }
  };
  walk(SRC);
  const hit = (needle) =>
    files
      // ★ 只看**代码**（`stripComments`）：`script_quickjs.cpp` 的头注里就**写着** `` `#include "quickjs.h"` ``
      //   这句解释 —— 把注释算进来会把它判成"第二个 include 点"（假红）。
      .filter((url) => stripComments(fs.readFileSync(url, 'utf8')).includes(needle))
      .map((url) => url.pathname.replace(/^.*\/src\//, 'src/'))
      .sort();
  // ★ `quickjs.h` 只许被**一个**头 include（`ui/quickjs_value.h` 是唯一边界；⛔ 不许第二个 TU）。
  assert.deepEqual(
    hit('#include "quickjs.h"'),
    ['src/ui/quickjs_value.h'],
    '`quickjs.h` 的 include 点必须**恰好**是 `src/ui/quickjs_value.h`（第二个 include 点 = 第二个 QJS TU）',
  );
  // ★ 那个边界头只许被**一个** TU 消费（`script_quickjs.cpp`）⇒ 全仓只有一个 TU 认识 JS 类型。
  assert.deepEqual(
    hit('#include "ui/quickjs_value.h"'),
    ['src/ui/script_quickjs.cpp'],
    '`ui/quickjs_value.h` 的消费者必须**恰好**是 `src/ui/script_quickjs.cpp`（否则 JS 类型面扩散）',
  );
  // ★ 反向自检：判据不是"walk 没读到文件"—— 至少要有几十个源文件，且必须包含那两条边的两端。
  assert.ok(files.length > 50, `walk 只读到 ${files.length} 个源文件（路径/扩展名写错了？）`);
  assert.ok(hit('#include "runtime/input.h"').length > 0, '反向自检：连 `runtime/input.h` 都读不到 ⇒ walk 坏了');
});
