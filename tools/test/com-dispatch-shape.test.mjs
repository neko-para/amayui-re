/** @env pure @kind gate @why COM dispatch 热路径又被塞回两个 std::string 拷贝 / known_this 又无条件查表 —— 每次 SetRenderState / SetSamplerState 白付一把锁 + 两次哈希 + 两次堆分配（读数见 .tmp/reveq/REQ-01M86NP8R79FE25CSMMV86JE31/report.md） */
/**
 * `ComRegistry::dispatch` 的**源码形态**守卫（纯文本，不构建、不跑 exe）。
 *
 * 背景（`REQ-01M86NP8R79FE25CSMMV86JE31`，2026-11-25）：COM vtable 方法的每次调用都要过
 * `ComRegistry::dispatch`。旧形状在**热路径**上付四笔固定税：
 *   ① 一把 `recursive_mutex`（读 `methods_`）；
 *   ② `methods_.find(stub)` 一次哈希；
 *   ③ `iface = e.iface; name = e.name;` 两个 `std::string` 拷贝 —— `iface` 16 字符 /
 *      `name` 33 字符都超过 MSVC 的 SSO 15 ⇒ **每次调用两次 malloc + 两次 free**（且在锁内）；
 *   ④ `objects_.find(call.u32(0))` 一次哈希 + 同一把锁 —— 结果**只喂一条 Info 日志**。
 * 本单的改法（C1–C4）：按槽平坦数组（`method_by_slot_`）+ 只留 `const MethodEntry*` +
 * `known_this` 按 `logVisible(Info, LogCom)` 门控。
 *
 * ★ 为什么这些值一条守卫（**它们全都静默**）：改回去不报错、不改 guest 语义，只是让每次调用
 *   重新付 0.1–0.2 µs；没有任何行为用例会红（耗时不是语义），读数又是 `.tmp/` 里的一次性产物
 *   （不进库）。所以判据只能是**源码形态**。
 * ★ 行为侧的另一半在 `tests/mechanism.cpp` 的 `com-dispatch-unknown-this`（那条 Info 诊断
 *   在 `--log-level info` 下仍必须**逐字**打出来）：本文件不替代它，两者守的是不同的一半
 *   （本文件守"税没回来"，它守"门控没把诊断关死"）。
 *
 * ⚠ 诚实边界：这是**源码谓词**。把拷贝藏进另一个 helper、或换一种等价的写法能绕过它；
 *   真正的兜底是 CTest 的 COM/D3D 行为用例（语义）与 `.tmp/` 报告里的读数（性能，一次性证据）。
 *   本文件只钉"最容易改回去的那几行"。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const ROOT = new URL('../../', import.meta.url);
const COM_CPP = 'apps/emulator-lift/src/runtime/com.cpp';
const COM_H = 'apps/emulator-lift/src/runtime/com.h';

function read(rel) {
  return fs.readFileSync(new URL(rel, ROOT), 'utf8');
}

/** 源码去掉 `//` 行注释（判据只看**活代码**：注释里出现 `std::string` / `iface = e.iface` 不算）。 */
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

const DISPATCH = () => bodyOf(codeOf(COM_CPP), 'void ComRegistry::dispatch(');

test('dispatch 热路径不拷贝 MethodEntry 的两个 std::string（C3）', () => {
  const body = DISPATCH();
  assert.ok(
    !body.includes('std::string'),
    'dispatch 函数体里又出现了 std::string —— 每个 COM 方法调用又会付两次 malloc/free（iface 16 / name 33 都超 SSO）',
  );
  // 两种写法都抓：`iface = e.iface`（旧形状）与 `const std::string iface = e->iface`（绕过上一条的写法）。
  assert.ok(
    !/(?:\biface\b|\bname\b)\s*=\s*e\s*(?:->|\.)/.test(body),
    'dispatch 又把 `e` 的 iface / name 拷进局部变量 —— 那是每次调用两次堆分配的那两行',
  );
});

test('dispatch 只经按槽平坦数组查方法（C1/C3）', () => {
  const body = DISPATCH();
  assert.ok(body.includes('methodByStubLocked('), 'dispatch 没有走 methodByStubLocked（按槽数组的快路径）');
  assert.ok(
    !body.includes('methods_.'),
    'dispatch 又直接碰 methods_（哈希查找 + 一把锁）—— 方法表只该经 methodByStubLocked 读',
  );
});

test('known_this 在 logVisible 之后（C4：不可见时不查 objects_）', () => {
  const body = DISPATCH();
  const gate = body.indexOf('logVisible(');
  assert.notEqual(gate, -1, 'dispatch 里没有 logVisible(...) 门（known_this 会每次调用都查 objects_ + 取锁）');
  const known = body.indexOf('known_this');
  assert.notEqual(known, -1, 'dispatch 里没有 known_this（那条"不是 ComRegistry 建出来的对象"的诊断丢了？）');
  assert.ok(known > gate, '`known_this` 出现在 logVisible(...) **之前** ⇒ 默认档下每次调用都白取锁 + 白查 objects_');
  const find = body.indexOf('objects_.find(');
  assert.ok(
    find === -1 || find > gate,
    '`objects_.find(` 出现在 logVisible(...) 之前 ⇒ 诊断不可见时仍然在做那次哈希 + 那把锁',
  );
});

test('com.h 里有按槽平坦数组与页参数（C1）', () => {
  const header = codeOf(COM_H);
  assert.match(
    header,
    /std::vector<const MethodEntry\*>\s+method_by_slot_;/,
    'com.h 里没有 `std::vector<const MethodEntry*> method_by_slot_;`（按槽平坦数组被删了？）',
  );
  assert.match(header, /std::uint32_t\s+stub_page_\s*=\s*0;/, 'com.h 里没有 stub_page_（页参数丢了 ⇒ 数组永远用不上）');
  assert.match(header, /std::uint32_t\s+stub_slots_\s*=\s*0;/, 'com.h 里没有 stub_slots_（页参数丢了）');
  assert.match(
    header,
    /const MethodEntry\*\s+methodByStubLocked\(\s*std::uint32_t\s+stub\s*\)\s*const;/,
    'com.h 里没有 `const MethodEntry* methodByStubLocked(std::uint32_t) const;` 的声明',
  );
});

test('registerSlot 在 emplace 成功后发布到数组（C2）', () => {
  const body = bodyOf(codeOf(COM_CPP), 'std::uint32_t ComRegistry::registerSlot(');
  assert.ok(body.includes('method_by_slot_'), 'registerSlot 不写 method_by_slot_ ⇒ 数组永远是空的（快路径恒 miss）');
  assert.ok(body.includes('stubSlots()'), 'registerSlot 没有从 ShimRegistry 取桩槽数（页参数从哪来？）');
  assert.ok(body.includes('method_by_slot_.assign('), 'registerSlot 没有在首次注册时按桩槽数建立数组');
});

test('methodByStubLocked：页内数组 + stub 二次校验，页外回 methods_（C1 的慢路径）', () => {
  const body = bodyOf(codeOf(COM_CPP), 'const ComRegistry::MethodEntry* ComRegistry::methodByStubLocked(');
  assert.ok(body.includes('method_by_slot_'), 'methodByStubLocked 没有读按槽数组');
  assert.ok(
    body.includes('e->stub == stub'),
    'methodByStubLocked 丢了 `stub_ea` 二次校验 —— 表过期（夹具直写 stub_ea）时会给出错答案',
  );
  assert.ok(
    body.includes('methods_.find'),
    'methodByStubLocked 没有 `methods_.find` 慢路径 —— 页外 stub 会被误判成"缺失"（夹具直写 stub_ea 的形态会 loud trap）',
  );
});
