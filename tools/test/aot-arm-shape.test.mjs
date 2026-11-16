/** @env pure @kind gate @why AOT 臂重新长出第三份语义实现（`--native-overrides`）· 逐帧日志回到缺省档（行数随帧数增长）· 或启动快照的区域表回到缺省档（28 行噪音）—— 三条都会让"缺省档的读数面"被静默改回去 */
/**
 * `apps/emulator-lift/src/aot/` 的**源码形态守卫**（纯文本，不构建、不跑 exe）。
 *
 * ## 为什么需要它
 * AOT 臂（`emulator-lift-aot`）**今天一条 CTest 都没有**：`CMakeLists.txt` 里没有一条 `add_test`
 * 跑它，全树也没有 `PASS_REGULAR_EXPRESSION`。所以下面两件事**没有任何运行期门禁**能发现：
 *
 * ① **`--native-overrides`**：它在派发边界按 EA 把一个 guest 函数体交给**宿主 C++ 重实现**，
 *    命中后直接写 `EAX/EIP/ESP`（`native_overrides.cpp:152-155`）。它不是 IR 替换、也不是解释器
 *    —— 是**第三份语义实现**。它**从不启用**（缺省空串、唯一赋值点、装配被 `if` 锁死），
 *    所以删掉它不该改变任何行为；反过来，一旦它被谁加回来（或加了第二条装配路径），
 *    同一份 `--appdata` / 同一份 pin 下的读数就不再代表"构建期 lift 出来的代码"。
 * ② **逐帧 `[frame] n=… hash=…`**：每次 Present 一行，缺省档 300 帧 = 300 行（占默认 stdout 的 78%）。
 *    它是画面序列读数（"在动 / 卡住"），不是判据；末帧 hash 另有一份（摘要 `present : … lastHash=`）。
 *    它的正确形态是 **opt-in**（`--frame-log`，或既有的 `--profile` 档顺带保留 —— 那是存档的可比面）。
 *
 * ## 它守不了什么（诚实边界）
 * ① 它不判"执行语义对不对"（那要真跑 AOT 臂，属于分钟级、要安装树的事，进不了 pure 档）；
 * ② 它不判输出**行数**（那是读数，写进 `.tmp/aot/ovr/impl-report.md`，⛔ 不进 test —— AGENTS §10）；
 * ③ 它按**标识符名字**判，所以改名能绕过。这是有意的取舍：正因为它是一次**删除**，
 *    名字就是删除面的清单；将来真要有新机制，应当先改这份清单（那是一次显式决定，不是静默回归）。
 *
 * 判据来源：`.tmp/aot/ovr/report.md`（该不该删：判"删"）与 `.tmp/aot/output/report.md`（输出面逐条定位）。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const ROOT = new URL('../../', import.meta.url);
const AOT_DIR = new URL('apps/emulator-lift/src/aot/', ROOT);
const AOT_MAIN = new URL('apps/emulator-lift/src/aot/aot_main.cpp', ROOT);
const AOT_RUN = new URL('apps/emulator-lift/src/aot/aot_run.cpp', ROOT);
const AOT_HOST_H = new URL('apps/emulator-lift/src/aot/aot_host.h', ROOT);
const AOT_HOST_CPP = new URL('apps/emulator-lift/src/aot/aot_host.cpp', ROOT);

function read(url) {
  return fs.readFileSync(url, 'utf8');
}

/** `src/aot/` 下的源码文件（`.cpp` / `.h`，不递归 —— AOT 臂是平铺的）。 */
function aotSources() {
  return fs
    .readdirSync(AOT_DIR)
    .filter((name) => name.endsWith('.cpp') || name.endsWith('.h'))
    .sort()
    .map((name) => ({ name, text: read(new URL(name, AOT_DIR)) }));
}

/**
 * ★ 2026-11-16 删掉的那套机制用到的**全部**标识符（按"删掉的符号"列，不是按"出现过"列）。
 *  判据是"这行代码只服务 AOT 臂的 override 机制"：共用件（`src/runtime/native_overrides.*` /
 *  `native_*.cpp` / `Machine::*` / 解释器测试）一行没动，所以它们仍可用那些名字 —— 本守卫只扫 `src/aot/`。
 */
const FORBIDDEN_OVERRIDE_TOKENS = [
  // ---- CLI / 帮助文本（`--lst` 在 AOT 臂只为 override 解析名字→EA，随它一起删）----
  '--native-overrides',
  '--lst',
  'native_overrides',
  'lst_path',
  // ---- 两张表 / 位图 / 嵌套项名单（`aot_host.h` 的私有面 + 装配函数）----
  'NativeOverrideTable',
  'NativeBlockTable',
  'configureNativeOverrides',
  'overrides_spec_',
  'overrides_',
  'block_overrides_',
  'override_filter_',
  'kOverrideFilterBits',
  'rebuildOverrideFilter',
  'overrideCandidate',
  'overrideCallFrame',
  'not_call_logged_',
  'nested_only_ea_',
  'nested_only_name_',
  'NestedOnlyOverride',
  'kNestedOnly',
  'findNestedOnly',
  // ---- `.lst` 装配（AOT 侧唯一消费者就是 override）----
  'loadLst',
  'lstLoaded',
  'lst_loaded_',
  'LstIndex',
  'lst_index.h',
  'native_overrides.h',
  // ---- 记账 / 输出（`AotRunResult` 的三个 `ov_*` 字段 + 三条 stdout 行）----
  'ov_insns',
  'ov_skipped_nested',
  'ov_not_call',
  'native-ovr',
  'override  :',
  // ---- 那句错措辞（AOT 没有解释器可回退；同族写的是"回退 lifted"）----
  '回退解释器',
];

test('① AOT 臂里不许再出现 override 机制的标识符（第三份语义实现不许回来）', () => {
  const hits = [];
  for (const { name, text } of aotSources()) {
    const lines = text.split('\n');
    for (const token of FORBIDDEN_OVERRIDE_TOKENS) {
      for (let i = 0; i < lines.length; i += 1) {
        if (lines[i].includes(token)) hits.push(`src/aot/${name}:${i + 1}: ${token}`);
      }
    }
  }
  assert.deepEqual(
    hits,
    [],
    `AOT 臂重新出现了 override 机制的标识符（它替换的是**执行来源**，不是 IR ⇒ 开了它，"跑通"` +
      `就不再证明 lifted 代码对；见 .tmp/aot/ovr/report.md）：\n  ${hits.join('\n  ')}`,
  );
});

test('② 逐帧 `[frame]` 必须在 `frame_log`（或 `prof_.on`）的条件里 —— 缺省档 0 行', () => {
  const text = read(AOT_RUN);
  const lines = text.split('\n');
  const prints = [];
  for (let i = 0; i < lines.length; i += 1) {
    if (lines[i].includes('std::printf("[frame]')) prints.push(i);
  }
  assert.ok(prints.length > 0, 'aot_run.cpp：找不到 `std::printf("[frame] …")`（逐帧读数被整段删了？）');
  // 闸门必须**紧挨着**那条 printf（同一行或前两行）：远在 lambda 头部的 early-return 不算 ——
  // 那样"把闸门去掉"是删一行，而 printf 与它的条件会脱钩（这正是本守卫要抓的形态）。
  for (const i of prints) {
    const window = lines.slice(Math.max(0, i - 2), i + 1).join('\n');
    assert.ok(
      /frame_log|prof_\.on/.test(window),
      `aot_run.cpp:${i + 1}：这条逐帧 \`[frame]\` printf 不在 frame_log / prof_.on 的条件里\n` +
        `  ⇒ 缺省档会每 Present 打一行（300 帧 = 300 行）。闸门要写成紧邻的 \`if (frame_log_on) {\`。\n` +
        `  附近源码：\n${window}`,
    );
  }
  // opt-in 的开关必须真的接上：`--frame-log` 有解析、有字段、有 --help 文案。
  const main = read(AOT_MAIN);
  const helpAndParse = (main.match(/--frame-log/g) ?? []).length;
  assert.ok(
    helpAndParse >= 2,
    `aot_main.cpp：\`--frame-log\` 只出现 ${helpAndParse} 次（至少要 --help 文案 + 一个解析分支）`,
  );
  assert.ok(
    /bool frame_log\s*=/.test(read(AOT_HOST_H)),
    'aot_host.h：`AotRunOptions` 里没有 `bool frame_log = false;`（`--frame-log` 的字段）',
  );
});

test('③ guest 区域表的打印必须在 `region_log`（`--region-log`）的条件内 —— 缺省档 0 行', () => {
  // ★ 判据来源：用户 2026-11-16 的口径"region 信息是多余的噪音，考虑 opt-in 而非默认"。
  //   `printReadout` 的 68 行里 **28 行**是区域表（`regions   :` 1 行 + 每段一行 27 行），
  //   它是一次性快照里**唯一被判为噪音**的一块（⛔ 别的一次性 readout 一格都不许收）。
  //   ⇒ 断言的是**性质**：那两条打印必须落在 `if (region_log) { … }` 的**块内**。
  //   ⛔ 不断言"输出里没有 `regions :`"：那会把"把整块删掉"也判成合格，而删掉等于丢判据面
  //   （要的是**保留读数、改成 opt-in**，不是让它消失）。
  const text = read(AOT_HOST_CPP);
  const lines = text.split('\n');
  const isComment = (s) => s.trim().startsWith('//');

  /** 从 `if (region_log) {` 那行起做花括号配对，返回 [起, 止] 行号（含）；找不到闸门 ⇒ null。 */
  function gateBlockRange() {
    for (let i = 0; i < lines.length; i += 1) {
      if (isComment(lines[i])) continue;
      if (!/if\s*\(\s*region_log\s*\)/.test(lines[i])) continue;
      let depth = 0;
      let opened = false;
      for (let j = i; j < lines.length; j += 1) {
        for (const ch of lines[j]) {
          if (ch === '{') {
            depth += 1;
            opened = true;
          } else if (ch === '}') {
            depth -= 1;
            if (opened && depth === 0) return [i, j];
          }
        }
      }
    }
    return null;
  }

  const gate = gateBlockRange();
  assert.ok(
    gate !== null,
    'aot_host.cpp：找不到 `if (region_log) { … }` 这块闸门 —— 区域表必须在 `region_log` ' +
      '（`--region-log`，缺省关）的条件内打印。`if (true)` / 无条件打印都会让缺省档回到 28 行。',
  );

  const prints = [];
  for (let i = 0; i < lines.length; i += 1) {
    if (isComment(lines[i])) continue;
    if (lines[i].includes('regions   :') || lines[i].includes('dumpRegions(')) prints.push(i);
  }
  assert.ok(
    prints.length >= 2,
    `aot_host.cpp：找不全区域表的两处打印（期望 \`regions   :\` 与 \`dumpRegions(\` 各一行，` +
      `实际 ${prints.length} 处）—— 区域表被整段删了？本守卫要的是"保留读数 + 改闸门"，不是删掉读数`,
  );
  for (const i of prints) {
    assert.ok(
      i > gate[0] && i < gate[1],
      `aot_host.cpp:${i + 1}：这条区域表打印在 \`if (region_log)\` 的块**外**（块 = ` +
        `第 ${gate[0] + 1}–${gate[1] + 1} 行）⇒ 缺省档会打整块 28 行快照。\n` +
        `  该行：${lines[i].trim()}`,
    );
  }
  // opt-in 的开关必须真的接上（与 ② 同一张清单）：`--region-log` 有解析、有字段（**缺省关**）、有 --help 文案。
  const main = read(AOT_MAIN);
  const helpAndParse = (main.match(/--region-log/g) ?? []).length;
  assert.ok(
    helpAndParse >= 2,
    `aot_main.cpp：\`--region-log\` 只出现 ${helpAndParse} 次（至少要 --help 文案 + 一个解析分支）`,
  );
  assert.ok(
    /bool region_log\s*=\s*false\s*;/.test(read(AOT_HOST_H)),
    'aot_host.h：`AotRunOptions` 里没有 `bool region_log = false;`（`--region-log` 的字段，且必须缺省关）',
  );
});
