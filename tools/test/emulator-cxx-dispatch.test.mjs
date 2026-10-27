/** @env pure @kind gate @why C++ 分派表与 handler 声明脱节时（声明了、定义了、却没有 case），opcode 会落进 default 抛"未实现"而编译链接全绿 —— 这正是"看起来实现了、其实没接上"的那类坏结果 */
/**
 * `apps/emulator-cxx` 的**分派表 ↔ handler 声明**一致性（纯文本，不编译）。
 *
 * 为什么需要它：`vm.h` 里声明 `void op_xx(const Instruction&)`、`vm_ops_*.cpp` 里定义它，
 * 但 `vm.cpp` 的 `execute()` 少一行 `case 0xXX:` —— **编译与链接都过**，运行期该 opcode 落进
 * `default` 抛 `opcode-not-implemented`。C++ 侧没有别的机械判据能发现这件事（缺定义才会链接失败）。
 *
 * ★ 这条守卫只认两件**可机械读出的东西**：声明集合 与 分派集合。它不管语义（语义在 selftest 与 spec）。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const ROOT = new URL('../../', import.meta.url);
const VM_H = new URL('apps/emulator-cxx/src/core/vm.h', ROOT);
const VM_CPP = new URL('apps/emulator-cxx/src/core/vm.cpp', ROOT);

/** ★ 本轮交付：boot → TITLE 的脚本链静态足迹里出现、原先缺 handler 的 17 条。
 *  判据见 `pnpm tools` 的足迹分析；这 17 条**必须**在分派表里（少一条 = 退回到"未实现"停点）。 */
const TITLE_PATH_OPCODES = [
  0x009, 0x0a1, 0x0a2, 0x0a3, 0x0d9, 0x0ff, 0x100, 0x12e, 0x19b, 0x19c, 0x1bf, 0x1ce, 0x1cf, 0x1f4,
  0x1f5, 0x208, 0x2fc,
];

/** ★ 下一段：TITLE 点 Option → CONFIG.BIN（id 52，设置界面）用到的 opcode 里**已经实现**的那些。
 *  这不是"CONFIG 的全部"（那由 CTest `emulator-core-config-screen` 判），而是"声明了就必须有 case"
 *  的棘轮：每实现一条就往这里加一条，⛔ 不要用它替代那条端到端 CTest。 */
const CONFIG_PATH_OPCODES = [
  0x06e, 0x06f, 0x0c0, 0x10d, 0x12f, 0x142, 0x196, 0x1a6, 0x1bb, 0x1d2, 0x1d3, 0x217,
  0x2cc, 0x2cd, 0x2ce, 0x2e6, 0x2e7, 0x2e8, 0x2ea, 0x300, 0x301,
  // ★ 设置界面（CONFIG1.BIN 20940 / CONFIG2.BIN 20939）右侧控件与整页重建走到的那批：
  //   显示模式请求 · 命名键 setter · 消息速度 · 自动消息显示时长读写 · 声道开关 · 音频请求族。
  0x2cf, 0x307, 0x1b5, 0x1b8, 0x1b9, 0x1ba, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xba,
];

function read(url) {
  return fs.readFileSync(url, 'utf8');
}

/** `vm.h` 里声明的 handler 名字（同名重载不存在，所以名字集合就是集合）。 */
function declaredHandlers() {
  const text = read(VM_H);
  const names = new Set();
  for (const match of text.matchAll(/void\s+(op_[0-9a-fA-F]+)\(const Instruction&\)/g)) {
    names.add(match[1]);
  }
  return names;
}

/** `execute()` 里的 (opcode → handler) 映射；同一行可以挂多个 `case`（分组写法）。 */
function dispatchTable() {
  const text = read(VM_CPP);
  const begin = text.indexOf('void Machine::execute');
  const end = text.indexOf('// 门之前的影片对象表服务');
  assert.ok(begin >= 0 && end > begin, 'vm.cpp：找不到 execute() 或它的结束标记');

  const table = new Map();
  for (const line of text.slice(begin, end).split('\n')) {
    const handler = line.match(/\b(op_[0-9a-fA-F]+)\(instruction\)/);
    if (!handler) continue;
    const cases = [...line.matchAll(/case\s+(0x[0-9a-fA-F]+|\d+)\s*:/g)].map((m) => Number(m[1]));
    if (cases.length === 0) continue; // 例如 `case 0x01: op_01(...)` 之外的特殊写法：不猜
    for (const opcode of cases) {
      assert.ok(!table.has(opcode), `opcode 0x${opcode.toString(16)} 在分派表里出现了两次`);
      table.set(opcode, handler[1]);
    }
  }
  return table;
}

test('★ 分派表 ↔ handler 声明：两个方向都不许漏', () => {
  const declared = declaredHandlers();
  const dispatch = dispatchTable();
  assert.ok(declared.size > 100, `声明的 handler 太少（${declared.size}）—— 解析器坏了？`);
  assert.ok(dispatch.size > 100, `分派表条目太少（${dispatch.size}）—— 解析器坏了？`);

  const dispatched = new Set(dispatch.values());
  const missingCases = [...declared].filter((name) => !dispatched.has(name)).sort();
  const missingDeclarations = [...dispatched].filter((name) => !declared.has(name)).sort();

  assert.deepEqual(
    missingCases,
    [],
    `这些 handler 有声明却没有 case（运行期会落进 default 抛"未实现"）：${missingCases.join(', ')}`,
  );
  assert.deepEqual(
    missingDeclarations,
    [],
    `这些 case 指向未声明的 handler（分派表写错了名字）：${missingDeclarations.join(', ')}`,
  );
});

test('★ 到 TITLE 用到的 17 条必须在分派表里', () => {
  const dispatch = dispatchTable();
  const missing = TITLE_PATH_OPCODES.filter((opcode) => !dispatch.has(opcode)).map(
    (opcode) => `0x${opcode.toString(16)}`,
  );
  assert.deepEqual(missing, [], `分派表缺这些 opcode：${missing.join(', ')}`);
});

test('★ CONFIG 路径上已实现的 opcode 不许漏 case', () => {
  const dispatch = dispatchTable();
  const missing = CONFIG_PATH_OPCODES.filter((opcode) => !dispatch.has(opcode)).map(
    (opcode) => `0x${opcode.toString(16)}`,
  );
  assert.deepEqual(missing, [], `分派表缺这些 opcode：${missing.join(', ')}`);
});
