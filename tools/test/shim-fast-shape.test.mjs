/** @env pure @kind gate @why shim 热路径的四个固定税被改回去了 —— 每次调用又白付一次 snprintf / 两个 std::string 拷贝 / 整份 DeviceInfo（15 次 guest 读）/ 32 次逐 float rd32（读数见 .tmp/shim-fast/report.md） */
/**
 * `apps/emulator-lift` shim 热路径的**源码形态**守卫（纯文本，不构建、不跑 exe）。
 *
 * 背景：2026-11-23 的 shim-fast 批次把四个固定税从热 shim 上砍掉（读数见
 * `.tmp/shim-fast/report.md`）：
 *   ① 每次调用一次 `snprintf` 前缀（`d3d9Log` / `d3dxLog` / `shimVPrintAt`）——
 *      默认 `warn` 下 Info 不可见，旧形状每次调用都白拼一次前缀；
 *   ② 每次调用一次整份 `ShimDef` 拷贝（两个 `std::string`）——
 *      `ShimCallDef` 只拷 POD，`module` / `name` 指向 `defs_`（`std::deque`，引用稳定）里的字符串；
 *   ③ 每次调用一次设备解析 —— `SetRenderState` / `SetSamplerState` 只要 `state` EA，
 *      不再填整份 `DeviceInfo`（15 次 guest 读 + 72 B 拷贝）；
 *   ④ 逐 float 的 guest 访问 —— `D3DXMatrixMultiply` 等先一次 `committed`、再整块 64 B 搬。
 *
 * ★ 为什么这些值一条守卫（**它们全都静默**）：改回去不报错、不改 guest 语义，只是让每次 shim
 *   调用重新付 0.2–0.5 µs 的固定税 —— 没有任何行为用例会红（耗时不是语义），而读数又是
 *   `.tmp/` 里的一次性产物（不进库）。所以判据只能是**源码形态**：门在拼串之前、POD 拷贝
 *   不含 `*out = src`、热路径不声明 `DeviceInfo`、`wrMat` 用 `writeBytes`。
 *
 * ⚠ 诚实边界：这是**源码谓词**。把逻辑藏进另一个 helper、或换一个等价的拼串方式，能绕过它；
 *   真正的兜底是 CTest 的 D3D9 / D3DX 行为用例（语义）与 `.tmp/shim-fast/report.md` 的读数
 *   （性能，一次性证据）。本文件只钉"最容易改回去的那几行"。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const ROOT = new URL('../../', import.meta.url);

function read(rel) {
  return fs.readFileSync(new URL(rel, ROOT), 'utf8');
}

/** 源码去掉 `//` 行注释（判据只看**活代码**：注释里出现 `*out = src` / `std::vector` 不算）。 */
function codeOf(rel) {
  return read(rel)
    .split('\n')
    .map((line) => {
      const cut = line.indexOf('//');
      return cut === -1 ? line : line.slice(0, cut);
    })
    .join('\n');
}

/** 取一个函数/方法的函数体（从签名后的第一个 `{` 做花括号配对；这些函数体里没有含 `{` 的字符串）。 */
function bodyOf(source, signature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `源文本里找不到 ${signature}`);
  const open = source.indexOf('{', start);
  assert.notEqual(open, -1, `${signature} 没有函数体`);
  let depth = 0;
  for (let i = open; i < source.length; i += 1) {
    if (source[i] === '{') depth += 1;
    else if (source[i] === '}') {
      depth -= 1;
      if (depth === 0) return source.slice(open, i + 1);
    }
  }
  assert.fail(`${signature} 的花括号不平衡`);
}

function assertGateBeforeSnprintf(file, signature, label) {
  const body = bodyOf(codeOf(file), signature);
  const gate = body.indexOf('logVisible(');
  const snp = body.indexOf('snprintf(');
  assert.ok(gate !== -1, `${label}：函数体里没有 logVisible(...) 门`);
  assert.ok(snp !== -1, `${label}：函数体里没有 snprintf(...) 前缀拼接`);
  assert.ok(gate < snp, `${label}：logVisible(...) 必须在 snprintf(...) 之前（默认档否则每次调用白拼前缀）`);
}

test('log 前缀门：可见性判定在 snprintf 之前', () => {
  assertGateBeforeSnprintf('apps/emulator-lift/src/shims/d3d9.cpp', 'void d3d9Log(', 'shims/d3d9.cpp d3d9Log');
  assertGateBeforeSnprintf('apps/emulator-lift/src/shims/d3dx9.cpp', 'void d3dxLog(', 'shims/d3dx9.cpp d3dxLog');
  assertGateBeforeSnprintf('apps/emulator-lift/src/shims/register.h', 'inline void shimVPrintAt(',
    'shims/register.h shimVPrintAt');
});

test('shim 热路径不再拷贝 ShimDef 的两个 std::string', () => {
  const body = bodyOf(codeOf('apps/emulator-lift/src/runtime/shims.cpp'), 'void ShimRegistry::prepareLocked(');
  assert.ok(!body.includes('*out = src'), 'prepareLocked 又变回整份 ShimDef 拷贝（每个 def 两个 std::string）');
  for (const field of [
    'out->module = src.module.c_str()',
    'out->name = src.name.c_str()',
    'out->reason = src.reason',
    'out->fn = src.fn',
    'out->stub_ea = src.stub_ea',
    'out->id = src.id',
    'out->conv = src.conv',
    'out->argc = src.argc',
  ]) {
    assert.ok(body.includes(field), `prepareLocked 少了 POD 字段赋值：${field}`);
  }
  const header = codeOf('apps/emulator-lift/src/runtime/shims.h');
  assert.match(header, /std::deque<ShimDef>\s+defs_;/, 'defs_ 必须是 std::deque<ShimDef>（ShimCallDef 的字符串指针要稳定）');
  assert.ok(!/std::vector<ShimDef>\s+defs_;/.test(header),
    'defs_ 不能退回 std::vector：push_back 扩容会让 ShimCallDef 里缓存的字符串指针悬垂');
});

test('SetRenderState / SetSamplerState 只解析 state EA（不再填整份 DeviceInfo）', () => {
  const src = codeOf('apps/emulator-lift/src/shims/d3d9.cpp');
  for (const fn of ['void shimSetRenderState(', 'void shimSetSamplerState(']) {
    const body = bodyOf(src, fn);
    assert.ok(body.includes('requireDeviceState('), `${fn} 没有走 requireDeviceState（热路径又填整份 DeviceInfo）`);
    assert.ok(!body.includes('d3d9::DeviceInfo dev;'), `${fn} 又声明了 DeviceInfo（15 次 guest 读 + 72 B 拷贝）`);
  }
});

test('D3DX math 整块搬 64 B（不再逐 float rd32/wr32）', () => {
  const src = codeOf('apps/emulator-lift/src/shims/d3dx9.cpp');
  const mul = bodyOf(src, 'void shimD3DXMatrixMultiply(');
  assert.ok(mul.includes('readMat(cpu, a_ea'), 'D3DXMatrixMultiply 没有整块读 a（64 B）');
  assert.ok(mul.includes('readMat(cpu, b_ea'), 'D3DXMatrixMultiply 没有整块读 b（64 B）');
  assert.ok(!mul.includes('rdF('), 'D3DXMatrixMultiply 又回到逐 float 的 rdF');
  const wr = bodyOf(src, 'void wrMat(');
  assert.ok(wr.includes('writeBytes(ea, d3d, sizeof(d3d))'), 'wrMat 没有整块写 64 B');
  assert.ok(!wr.includes('for (int i = 0; i < 16'), 'wrMat 又回到 16 次 wr32');
});
