/** @env pure @kind gate @why 输入投递层 S1 的两条形状判据掉了 —— ① 执行循环里又长出一个投递/推进调用点（"块边界 pump"回来了）· ② 有一条读 guest 输入态/消息队列的已注册 shim 没经投递入口（漏一条读路径） */
/**
 * `apps/emulator-lift` 输入**投递层** S1 的源码形态守卫（纯文本，不构建、不跑 exe）。
 *
 * 引用单：`REQ-01M7G7EMX3JR272D8FVM7JFY9D`（父单 `REQ-01M7G7EJF062NKQ0D40RF3BZ4Z`）。
 * 设计：`.tmp/aot/input/design.md`（⚠ 有错）· 复审：`.tmp/aot/input-review/review.md`（逐条纠错）。
 *
 * ★ **2026-11-27（用户裁决 `REQ-01M875NDRETPPNN68VCE3VMCT1`，`--input-plan` 整体退役）**：
 *   旧的 ③（"关系断言不是恒真"—— 钉 `tests/input_plan.cpp` 的 `deliverCalls == ShimDef::call_count`
 *   与 `add_test(NAME input-plan-read-points …)`）**已删**：那个 CTest 与源文件本批一起退场
 *   （`input-plan-shape` 的 ③ 没有对象了）。①/② 保持不变 —— ② 判的投递层今天仍服务
 *   `ReadPointObserver`（`input.h` 的 `input::deliver*`）与控制层立即队列（`ControlInput::atPoint`），
 *   与旧 `InputPlan` 类是否存在无关。
 *   ★ ① 的禁用词随类删除**缩表**：`InputPlan` / `input_plan` / `note(` 已不存在于本仓
 *   （删掉它们不是放宽判据 —— 目标词本身没了）；`input::` / `deliver(` / `atPoint(` 等
 *   "把推进塞回循环"的词**保留**（真正要防的回潮形态）。
 *
 * ## 两条判据（红了意味着什么）
 * ① **执行循环里不许出现投递层**：`src/runtime/machine.cpp` / `src/aot/aot_run.cpp` 里
 *    `input::` / `deliver(` / `atPoint(` / `TriggerRunner` / `onTrigger(` / `IntentExecutor` 命中 = 0。
 *    ★ 2026-11-27：旧 `InputPlan` 的三条词（`InputPlan` / `input_plan` / `note(`）随类删除退场
 *      —— 它们在本仓已不存在，留着只会变成"永远不可能命中"的假守卫。防的是**形态**：任何把
 *      输入推进塞回执行循环 / 块边界的写法都会撞上 `input::` / `deliver(` / `atPoint(`。
 *    ★ 旧 `harness_script` 闸门**本轮不删**（那是 S6 的事）。所以 `tickInputScript` /
 *      `tickInteraction` 在 `machine.cpp` 里**允许**存在于两处老位置：函数定义行，或
 *      `if (harness_marker || harness_script) { … }` 的块内。任何新位置（尤其 `aot_run.cpp`
 *      与循环体别处）都红 ⇒ 判据是"新路不碰循环"，而不是"今天把旧路删掉"。
 *
 * ② **读点闭环（按谓词，不按名单）**：对 `src/shims/` 里**所有**已注册 shim（`s.add` /
 *    `s.addStub`，即有实现的那些）逐个提取函数体，判两个**互相独立**的谓词：
 *      A) "读 guest 可见输入态或消息队列" ⟺ "函数体里调了 `input::deliverLevel/deliverQueue/deliverWrite`"
 *      B) "真的从文件读字节（`std::fread(`）" ⟺ "函数体里调了 `input::deliverFileRead`"
 *    两侧都不许单边为真。⛔ 这不是"11 个名字必须在表里"：`SetCursor` / `ShowCursor` 注册了但
 *    谓词为假、也没有投递调用 ⇒ 两侧一致；`GetQueueStatus` 今天返回值恒 0，但函数体读
 *    `queueSize()`（谓词为真）且调了 `deliverQueue` ⇒ 两侧一致。
 *    ★ 诚实边界（与 `aot-arm-shape.test.mjs:21` 同口径）：这是**源码谓词**，所以
 *      "把读操作藏进另一个 helper"能绕过。真正补它的是"未注册 ⇒ 响亮 trap"（`shims.cpp`
 *      的 `trapUnimplemented`）与 S2 之后的行为用例。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const ROOT = new URL('../../', import.meta.url);
const MACHINE_CPP = new URL('apps/emulator-lift/src/runtime/machine.cpp', ROOT);
const AOT_RUN_CPP = new URL('apps/emulator-lift/src/aot/aot_run.cpp', ROOT);
const SHIMS_DIR = new URL('apps/emulator-lift/src/shims/', ROOT);

function read(url) {
  return fs.readFileSync(url, 'utf8');
}

function linesOf(url) {
  return read(url).split('\n');
}

function hitLines(lines, tokens) {
  const hits = [];
  for (let i = 0; i < lines.length; i += 1) {
    for (const token of tokens) {
      if (lines[i].includes(token)) hits.push({ line: i + 1, token, text: lines[i].trim() });
    }
  }
  return hits;
}

/** 从含 `{` 的起始行做花括号配对（跳过 `//` 行注释里的括号：源码里没有字符串含花括号的形态）。 */
function braceBlockRange(lines, startIndex) {
  let depth = 0;
  let opened = false;
  for (let i = startIndex; i < lines.length; i += 1) {
    const code = lines[i].split('//')[0];
    for (const ch of code) {
      if (ch === '{') {
        depth += 1;
        opened = true;
      } else if (ch === '}') {
        depth -= 1;
        if (opened && depth === 0) return [startIndex, i];
      }
    }
  }
  return null;
}

// =====================================================================================
// ① 执行循环里 0 命中
// =====================================================================================
// ★ 2026-11-20（C1b）追加后六个：插件运行时的 **唯一 pump** 是 `TriggerRunner::atPoint`（合同 §6 行 2），
//   而它的驱动方只能是 executor 的读点（C4 才接线 ⇒ 今天生产路径零改动）。执行循环里出现
//   `TriggerRunner` / `onTrigger(` = 第二 pump / 块边界触发；出现 `IntentExecutor` / `intent_executor`
//   = 把 pump 塞回了 `aot_run.cpp`。实测加词前这两个文件 **0 命中**（`rg -n` 逐个核过）。
//   ⛔ 不加 `fire(`：`hitLines` 用的是无词边界的 `includes`，而且本设计里 pump 的入口叫 `atPoint`、
//   **没有** `fire(` 这个公开拼写（`fire` 是 `atPoint` 内的局部步骤）⇒ 加了只会制造假红。
const LOOP_FORBIDDEN = [
  'input::', 'deliver(', 'atPoint(',
  'TriggerRunner', 'TriggerHandler', 'trigger_runner', 'onTrigger(',
  'IntentExecutor', 'intent_executor',
];

test('① 执行循环里 0 命中投递层标识符（旧 harness 闸门按"位置"豁免，不是按词豁免）', () => {
  for (const [label, url] of [
    ['src/runtime/machine.cpp', MACHINE_CPP],
    ['src/aot/aot_run.cpp', AOT_RUN_CPP],
  ]) {
    const hits = hitLines(linesOf(url), LOOP_FORBIDDEN);
    assert.deepEqual(
      hits.map((h) => `${label}:${h.line}: ${h.token}  ← ${h.text}`),
      [],
      `执行循环里出现了投递层标识符（用户口径：输入是外部数据，⛔ 不许在解释循环里判断 / 推进）：\n  ` +
        hits.map((h) => `${label}:${h.line}: ${h.token}  ← ${h.text}`).join('\n  '),
    );
  }

  // 旧 `harness_script` 闸门（S6 才删）：两个 tick 只许留在**两个老位置**。
  const lines = linesOf(MACHINE_CPP);
  const gateIndex = lines.findIndex((l) => /if\s*\(harness_marker \|\| harness_script\)\s*\{/.test(l));
  assert.ok(gateIndex >= 0, 'machine.cpp：找不到 `if (harness_marker || harness_script) {`（旧闸门被整段删了？S6 才做这件事）');
  const gate = braceBlockRange(lines, gateIndex);
  assert.ok(gate !== null, 'machine.cpp：`if (harness_marker || harness_script)` 的花括号没配对上');
  const tickHits = hitLines(lines, ['tickInputScript', 'tickInteraction']);
  const stray = [];
  for (const h of tickHits) {
    const isDefinition = /^\s*void Machine::tick(InputScript|Interaction)\(X86& cpu\)\s*\{/.test(lines[h.line - 1]);
    const inGate = h.line - 1 > gate[0] && h.line - 1 < gate[1];
    if (!isDefinition && !inGate) stray.push(h);
  }
  assert.deepEqual(
    stray.map((h) => `machine.cpp:${h.line}: ${h.token}  ← ${h.text}`),
    [],
    '旧 tick 出现在 harness 闸门之外（= 又有一个块边界 / 循环里的输入推进点）：\n  ' +
      stray.map((h) => `machine.cpp:${h.line}: ${h.token}  ← ${h.text}`).join('\n  '),
  );
  const aotTick = hitLines(linesOf(AOT_RUN_CPP), ['tickInputScript', 'tickInteraction']);
  assert.deepEqual(aotTick, [], 'aot_run.cpp 里出现了旧 tick（AOT 臂从来不该有输入脚本路径）');
});

// =====================================================================================
// ② 读点闭环（按谓词）
// =====================================================================================
const READ_TOKENS = [
  '->keyDown(', // 影子键电平（GetAsyncKeyState / TranslateMessage 的 shift / caps）
  '->getCursorPos(', // 虚拟光标（GetCursorPos）
  '->cursorClientPos(', // 光标反函数（SetCursorPos 的 warp 腿；写点也要接线）
  '->peekMatching(', // 队列过滤窥视（PeekMessageA）
  '->peek(', // 队列窥视（host_window 的 get 内部走它；留作口径完整）
  '->peekAt(', // 队列按 index 扫（MsgWaitForMultipleObjects）
  '->queueSize(', // 队列长度（MsgWait / GetQueueStatus）
  '->inputSampleCount(', // 采样心跳（任何读它的 shim 都算输入读者）
  '->noteInputSample(', // 采样计数（两个电平读点）
  '->mouseButtons(', // 按钮位图
  '->pumpTimers(', // 计时器消息 -> 消息队列（Peek / GetMessage / MsgWait / WaitMessage）
  'windows->get(', // 消息队列取一条（GetMessageA）
  'readMsg(', // 读 guest 的 MSG 结构（Translate / Dispatch：队列消费端）
  'writeMsg(', // 写 guest 的 MSG 结构（Peek / GetMessage）
];
const INPUT_DELIVERY = ['input::deliverLevel(', 'input::deliverQueue(', 'input::deliverWrite('];
const FILE_READ_TOKEN = 'std::fread('; // "真的从文件读字节" ⇒ `ReadFile` 这个事件源
const FILE_HANDLE_TOKEN = 'requireHandles('; // …而且读的是 **kernel32 文件句柄**（⛔ 不是 winmm 的 HMMIO）
const FILE_DELIVERY = 'input::deliverFileRead(';

// 注册行：`s.add("mod","Name",Conv::Xxx, N, &fn);` / `s.addStub(..., &fn, "reason");`
const REG_RE = /\bs\.add(?:Stub)?\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*Conv::\w+\s*,\s*\d+\s*,\s*&?([A-Za-z_]\w*)\s*[,)]/g;

/** 提取 `void fn(...) { … }` 的函数体（含花括号）；找不到 ⇒ null。 */
function functionBody(text, fn) {
  const re = new RegExp(`\\bvoid\\s+${fn}\\s*\\(`);
  const m = re.exec(text);
  if (!m) return null;
  const open = text.indexOf('{', m.index);
  if (open < 0) return null;
  let depth = 0;
  for (let i = open; i < text.length; i += 1) {
    const ch = text[i];
    if (ch === '{') depth += 1;
    else if (ch === '}') {
      depth -= 1;
      if (depth === 0) return text.slice(open, i + 1);
    }
  }
  return null;
}

test('② 读点闭环：按谓词枚举所有已注册 shim，"读输入/队列" ⟺ "调了投递入口"', () => {
  const files = fs
    .readdirSync(SHIMS_DIR)
    .filter((name) => name.endsWith('.cpp'))
    .sort();
  const registered = [];
  const noBody = [];
  for (const name of files) {
    const text = read(new URL(name, SHIMS_DIR));
    REG_RE.lastIndex = 0;
    let m;
    while ((m = REG_RE.exec(text)) !== null) {
      const body = functionBody(text, m[3]);
      if (body === null) noBody.push(`src/shims/${name}: ${m[2]} → ${m[3]}（找不到函数体）`);
      registered.push({ file: name, module: m[1], api: m[2], fn: m[3], body: body ?? '' });
    }
  }
  assert.deepEqual(noBody, [], `已注册 shim 的函数体找不到（解析器失配）：\n  ${noBody.join('\n  ')}`);
  // ★ 防"解析器整体失配 ⇒ 0 条 ⇒ 恒绿"的**地板**（不是精确计数断言；见 AGENTS.md §9）。
  assert.ok(
    registered.length > 100,
    `只解析出 ${registered.length} 条已注册 shim —— 注册语法变了 / 解析器失配（守卫会静默变绿）`,
  );

  const mismatches = [];
  for (const r of registered) {
    const readsInput = READ_TOKENS.some((t) => r.body.includes(t));
    const deliversInput = INPUT_DELIVERY.some((t) => r.body.includes(t));
    if (readsInput !== deliversInput) {
      mismatches.push(
        `src/shims/${r.file}: ${r.module}!${r.api} — 谓词"读 guest 可见输入态/消息队列"=` +
          `${readsInput}，函数体里调了投递入口=${deliversInput}`,
      );
    }
    const readsFile = r.body.includes(FILE_READ_TOKEN) && r.body.includes(FILE_HANDLE_TOKEN);
    const deliversFile = r.body.includes(FILE_DELIVERY);
    if (readsFile !== deliversFile) {
      mismatches.push(
        `src/shims/${r.file}: ${r.module}!${r.api} — 谓词"真的从文件读字节"=${readsFile}，` +
          `函数体里调了 deliverFileRead=${deliversFile}`,
      );
    }
  }
  assert.deepEqual(
    mismatches,
    [],
    '读点闭环断了（漏一条读路径，或给非读者硬塞了投递）：\n  ' + mismatches.join('\n  '),
  );
});
