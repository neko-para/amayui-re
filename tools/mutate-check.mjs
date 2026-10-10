#!/usr/bin/env node
/**
 * tools/mutate-check.mjs —— **守卫自检**：改坏一个常量，确认对应守卫**当场红**
 *
 * ## 为什么需要它
 * 本仓的纪律是"守卫要**红得有意义**"。但"写了守卫"和"守卫真的会红"是两件事 ——
 * 一个恒真的断言、一个把 `expected` 抄成 `actual` 的断言，都能"一直绿"。
 * 本工具用**变异测试**把这件事变成可复跑的判据：对每个关键常量施加一处已知的破坏，
 * 要求**指定的守卫文件退出码非 0**。
 *
 * ## 安全口径（★ 动的是工作树里的文件，所以必须能还原）
 * 1. 施加前后都读一遍原文；**原文进内存**，`finally` 里写回（异常路径也还原）。
 * 2. 还原后**再读一遍比对**（不只看长度 —— 比对全文）。
 * 3. 任何一步不符就**立刻中止**并报告，不继续跑下一个变异（避免"半坏的工作树"）。
 * 4. 不联网、不写别的文件、不改旧仓。
 *
 * 用法：`pnpm test:mutation`（也可 `node tools/mutate-check.mjs --list` 只看变异清单）
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

export const DOMAIN = {
  id: 'mutate',
  title: '守卫自检（变异测试）：改坏一处关键常量 ⇒ 确认指定守卫**当场红**',
  data: [
    '**工作树里被守卫覆盖的源文件**（`packages/age-format/src/asm/*.mts` · `apps/emulator/src/model/*.ts`）—— ★ **临时改写后必还原**',
    '`tools/mutate-check.mjs` 里的**变异清单**（本工具的真源：一处破坏 + 该抓它的守卫）',
  ],
  access: 'r（读原文进内存）→ rw（写入变异）→ **w（还原，且比对全文）**；★ 不写别的文件、不碰旧仓',
  tool: 'tools/mutate-check.mjs',
};

export const OPERATIONS = [
  { name: 'check', argv: [], mutates: true, summary: '★ 逐条施加变异 ⇒ 跑指定守卫 ⇒ 要求退出码非 0，最后**逐条还原并比对全文**' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '只列变异清单（不动任何文件）' },
];

export { MUTATIONS };

/**
 * 变异清单：每条 = 一处"已知的破坏" + 应该抓住它的守卫。
 * ★ 加新守卫时顺手加一条 —— 否则"这个守卫会红"只是个声称。
 */
const MUTATIONS = [
  // ── 数组类 opcode 的目标步长（★ 写死 4 会让 28 字节族算到别的元素上；语料里 723 个站点）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: '    case 0x8: case 0xe: return 28;',
    to: '    case 0x8: case 0xe: return 4;',
    guard: 'tools/test/emulator-pool-regions.test.mjs',
    what: '`0x61`/`0x12c` 对 28 字节族也用 4 字节步长 ⇒ 字符串指针会指到错元素（而"读到一个字符串"看起来正常）',
  },
  // ── 台账覆盖率棘轮（★ 模拟"新增一条知识层登记却不配台账条目"）──
  {
    file: 'packages/age-format/src/engine/layout.mts',
    from: "  { name: 'Engine.d517', dword: 517, opcode: 0xfe,",
    to: "  { name: 'Engine.d999', dword: 999, opcode: 0x999, handler: 'sub_999999', form: 'op1' },\n  { name: 'Engine.d517', dword: 517, opcode: 0xfe,",
    guard: 'tools/test/ledger.test.mjs',
    what: '知识层凭空多一条 opcode（`0x999`）而台账没有它 ⇒ 覆盖率比率下降（这正是"结论只写在注释里"的机械化）',
  },
  // ── `0x6 load-frame` 的帧深上限（★ 去掉它 ⇒ 引擎会抛的那条变成"照跑不误"）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: '  if (slot >= FRAME_SLOT_COUNT) {',
    to: '  if (false) {',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`load-frame` 去掉帧深上限 ⇒ 引擎抛「階層が深すぎます」那条变成无声通过',
  },
  // ── `0x6 load-frame` 的**槽语义**（★★ 这一条是本轮重构的核心判据）──
  //   `loadFrameAt` 顺手把 `cur` 也改掉 ⇒ "往槽 op2 写记录、装完**恢复**旧 cur"当场不成立：
  //   表现是"当前帧被换成了刚装的那份"（主循环会在错的帧上继续跑），而日志看起来一切正常。
  {
    file: 'apps/emulator/src/vm/machine.ts',
    from: '    while (this.slots.length <= slot) this.slots.push(null);',
    to: '    while (this.slots.length <= slot) this.slots.push(null);\n    this.cur = slot;',
    guard: 'tools/test/emulator-frames.test.mjs',
    what: '`loadFrameAt` 顺手改 `cur`（= 把"装完恢复旧 cur"实现成"切过去就不回来了"）⇒ 槽语义与"cur 不变"那条判据必须当场红',
  },
  // ── 越界口径：`0x107` 是"静默跳过"、`0x30a` 是"抛"（★ 统一它们会把"引擎会崩"与"无事发生"变成同一种表现）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: "      if (spec.outOfRange === 'skip') {",
    to: '      if (true) {',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '把所有标量数组的越界都当成"静默跳过" ⇒ 引擎会抛的那条（0x30a）变成了无声无息',
  },
  // ── 字符串指针的定位（★ 步长必须是 28：用 4 会静默定位到**另一个元素**）──
  {
    file: 'apps/emulator/src/vm/operand.ts',
    from: '  const idx = off / hit.elemBytes;',
    to: '  const idx = off / 4;',
    guard: 'tools/test/emulator-host.test.mjs',
    what: '字符串元素的定位用 4 字节步长（应当是 28）⇒ 静默定位到别的元素，而"读到了一个字符串"看起来很正常',
  },
  // ── float 族迁区域：格内容 = **位模式**，读的时候必须换算回数值 ──
  {
    file: 'apps/emulator/src/model/pools.ts',
    from: "  if (def.kind === 'float') return floatFromBits(bits);",
    to: "  if (def.kind === 'float') return bits;",
    guard: 'tools/test/emulator-pool-regions.test.mjs',
    what: 'float 池读回来的是**位模式**（1067450368）而不是数值（1.25）—— 用错的地方会"看起来是个整数"',
  },
  // ── 标量数组：索引**必须**进键名（★ 否则两次不同的写互相覆盖，而日志看不出异常）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: '    const key = `${spec.name}+${idx}`;',
    to: '    const key = spec.name;',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '标量数组的索引不进键名 ⇒ 第 3 格与第 4 格的写互相覆盖，且快照只看到一个键',
  },
  // ── 引擎自己的范围检查（★ 改成 clamp 会把"脚本写错了"变成"值变了一点"）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: '      if (capped !== undefined && value > capped) {',
    to: '      if (false && capped !== undefined && value > capped) {',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '知识层登记的 `max`（引擎自己抛异常的那条）被忽略 ⇒ 越界值静默写进去',
  },
  // ── `0x64` 的块偏移（★ 少一个 +4 就会把"元素个数"当成第一个元素）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: 'encInt(u32(blockOff + 4 + 4 * i), key)',
    to: 'encInt(u32(blockOff + 4 * i), key)',
    guard: 'tools/test/emulator-pool-regions.test.mjs',
    what: '`copy-local-array` 的数据起点少算 4 字节 ⇒ 把"元素个数"当成第一个元素写进去',
  },
  // ── 指针族的语义（★ 错一处就会"解引用一个编码过的数"，而两条路都看起来正常）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: '  writeOperand(operandCtx(ctx), dest, 0, addr);',
    to: '  writeOperand(operandCtx(ctx), dest, 0, ctx.machine.space.readU32(addr) ?? 0);',
    guard: 'tools/test/emulator-pool-regions.test.mjs',
    what: '`lookup-array` 往指针格写**那一格的值**而不是**它的地址** ⇒ 下一步会去解引用那个值',
  },
  {
    file: 'apps/emulator/src/vm/operand.ts',
    from: '    const target = (addr ?? 0) >>> 0;',
    to: '    const target = (decInt((addr ?? 0) >>> 0, isLocal ? ctx.locals.key : ctx.globals.key) >>> 0);',
    guard: 'tools/test/emulator-pool-regions.test.mjs',
    what: '指针读**先 DEC 再解引用**（顺序反了）⇒ 会去解引用一个编码过的地址（逐字是"取格→解引用→DEC"）',
  },
  // ── 区域窗口（★ 本仓实测踩过两次静默串数据：容量 0 同基址；增长吞掉邻居）──
  {
    file: 'apps/emulator/src/model/address-space.ts',
    from: 'this.baseCursor = base + span;',
    to: 'this.baseCursor = base + r.byteLength;',
    guard: 'tools/test/emulator-pool-regions.test.mjs',
    what: '按当前字节长推进基址（而不是整个窗口）⇒ 容量 0 的区域同基址、增长还吞邻居 = **静默串数据**',
  },
  // ── 按需增长：**越界不许变成"悄悄扩容"**（★ 这一塌，引擎的 UB 就被伪装成了一种语义）──
  {
    file: 'apps/emulator/src/model/address-space.ts',
    from: '    if (need <= region.capacity) return false;',
    to: '    if (need <= region.capacity) return false;\n    if (need > region.capacity) { region.growTo(need); }',
    guard: 'tools/test/emulator-address-space.test.mjs',
    what: '越界访问顺手扩容（且不经过留痕钩子）⇒ 静默把 UB 放过去，日志里看不到任何增长',
  },
  // ── `0x76` 的 bswap24（★ 写成原值 = 颜色通道反了，而没人会报错）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: 'const bswap24 = (v: number): number => (((v & 0xff) << 16) | (v & 0xff00) | ((v >>> 16) & 0xff)) >>> 0;',
    to: 'const bswap24 = (v: number): number => v >>> 0;',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: 'bswap24 变成恒等 ⇒ `0x76`/`0x77` 写进去的是未交换的值（颜色通道反了，没有任何报错）',
  },
  // ── 字体表查找的"未命中 ⇒ -1"（★ 写成 0 会让"没找到"与"第 0 个"混起来）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: 'const value = index < 0 ? -1 : index;',
    note: '（先放这儿：bswap24 的条目在下面）',
    to: 'const value = index < 0 ? 0 : index;',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`0x2de` 未命中时给 0（= "第 0 个字体"）而不是 -1 ⇒ 脚本找不到字体却以为找到了',
  },
  // ── 配置文件的版本行（★ 不检查它 ⇒ 旧格式被静默当空配置 = "配置丢了"看起来像"第一次运行"）──
  {
    file: 'apps/emulator/frontends/headless/file-config.ts',
    from: '  if (lines[0] !== CONFIG_HEADER) {',
    to: '  if (false) {',
    guard: 'tools/test/emulator-file-config.test.mjs',
    what: '解析时不校验版本行 ⇒ 认不出的格式被静默当成空配置',
  },
  // ── load-string 的"查不到 ⇒ 空串"（★ 写成 "0" 会让配置缺失与"值是 0"混起来）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: "  const text = stored ?? '';",
    to: "  const text = stored ?? '0';",
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`load-string` 查不到时给 "0" 而不是空串 ⇒ "配置里没有这一项" 被静默变成 "值是 0"',
  },
  // ── fill-zero 的"填的不是 0"（★ 助记名骗人；日志跟着骗就没人能发现）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: "filled: 'encZero（DEC 后是 0；float 池按 float32 解释）',",
    to: "filled: '0',",
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`fill-zero` 的记录里不写"填的是 encZero" ⇒ 日志与助记名一起骗人（真键下 int/float 池会分叉）',
  },
  // ── 内联字符串（type 2）的偏移口径（★ 与 label 同一算式，错一格就解出另一段字节而**不报错**）──
  {
    file: 'apps/emulator/src/vm/script.ts',
    from: 'const offset = script.headerLen + (raw >>> 0) * 4;',
    to: 'const offset = (raw >>> 0) * 4;',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '内联字符串的偏移漏掉 headerLen ⇒ 每次跳转/每次读串都偏 15 条指令（60 B / 4）',
  },
  // ── 配置的键（★ 键拼错 ⇒ 读到的永远是"查不到"，而日志看着一切正常）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: "return `\\u0003${(value >>> 0).toString(16).padStart(8, '0')}`;",
    to: "return `\\u0003${(value >>> 0).toString(16)}`;",
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '配置键不补零（`%8.8x` 的"8 位"丢了）⇒ 键与引擎不一致，永远查不到（而日志一切正常）',
  },
  // ── 未建模的东西**不许**被记成"做了"（★ 这是本批最容易发生的自欺）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: '      applied: false,\n      reason:',
    to: '      applied: true,\n      reason:',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '对象字段写记成 `applied: true`（对象根本不在场）—— 日志会让人以为这些写发生了',
  },
  // ── 引擎标量堆与前段那批 setter（★ 塌了会静默：值丢了/形态错了，都没人报错）──
  // ── 条件位副作用（★ `0x71` **置** bit27 / `0x88` 在 `op1 == 0` 时**清**它，两条成对）──
  {
    file: 'packages/age-format/src/engine/layout.mts',
    from: "when: 'op1==0', name: 'Engine.d174801', dword: 174801, op: 'clear',",
    to: "when: 'op1==0', name: 'Engine.d174801', dword: 174801, op: 'set',",
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`0x88` 假支的**位清除**被写成**置位** ⇒ "`0x71` 置位后 `0x88` 能把它清掉"当场不成立（位只增不减，而日志看起来正常）',
  },
  {
    file: 'packages/age-format/src/engine/layout.mts',
    from: "when: 'always', name: 'Engine.d174801', dword: 174801, op: 'set', mask: 0x08000000 },",
    to: "when: 'always', name: 'Engine.d174801', dword: 174800, op: 'set', mask: 0x08000000 },",
    guard: 'tools/test/emulator-engine-scalars.assets.test.mjs',
    what: '★ 语料锚版：`0x71` 置位的槽号 174801 → 174800（字节偏移 `0AAB44h` 抄成 `0AAB40h`）⇒ `.lst` 里那条 `or [ebx+0AAB44h], 8000000h` 对不上',
  },
  // ── 转发的**完整实参**（★ `0x70` 给 callee 传了一个**写死的 0**；只记操作数 ⇒ 那条 `if (a7 >= 0)`
  //    分支在欠账里"永远看不见"，与"走了但没记录"变成同一种表现）──
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: "{ kind: 'operand', index: 4 }, { kind: 'const', value: 0 }] },",
    to: "{ kind: 'operand', index: 4 }, { kind: 'operand', index: 4 }] },",
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`0x70` 的**常量实参 0** 被当成又一个操作数（= 退回"只记操作数"的旧口径）⇒ "第 6 个显式实参是 0"这条当场红',
  },
  // ── **尾跳 vs 调用**（★ 四行原先 `callsAfter:['sub_459F40']` 长得一样；把尾跳写成 call ⇒
  //    "控制流交给它、本 handler 到此结束"被伪装成"调完会回来接着跑"）──
  {
    file: 'packages/age-format/src/engine/layout.mts',
    from: "{ name: 'Engine.d21667', dword: 21667, opcode: 0x78, handler: 'sub_41F450', form: 'op1', callsAfter: [{ callee: 'sub_459F40', transfer: 'tail',",
    to: "{ name: 'Engine.d21667', dword: 21667, opcode: 0x78, handler: 'sub_41F450', form: 'op1', callsAfter: [{ callee: 'sub_459F40', transfer: 'call',",
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`0x78` 的**尾跳**（逐字 `.text:0041F47F jmp sub_459F40`）被写成 `call` ⇒ "欠账里分得出尾跳与调用"当场不成立',
  },
  {
    file: 'apps/emulator/src/model/engine-scalars.ts',
    from: 'return this.values.get(name) ?? 0;',
    to: 'return this.values.get(name) ?? 1;',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '没写过的标量槽返回 1 而不是 0 —— 引擎把 Engine+0x5EC9C..0x5ECE8 那片清零了（逐字 .lst:33974-33994；台账 subject Engine+0x5EC9C..0x5ECE8/ctor-zero-fill）',
  },
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: "w.form === 'bool(op1)' ? (v !== 0 ? 1 : 0)",
    to: "w.form === 'bool(op1)' ? v",
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`bool(op1)` 形态不归一（写原值 7 而不是 1）—— 知识层登记的形态被静默忽略',
  },
  // ── opcode→handler 的机械查询（★ 塌了会让实现者按错的符号去读函数体）──
  {
    file: 'tools/lib/opcodes.mjs',
    from: 'export const DISPATCH_BASE = 0xa509c;',
    to: 'export const DISPATCH_BASE = 0xa50a0;',
    guard: 'tools/test/opcodes-handlers.assets.test.mjs',
    what: '分派表基址挪一槽（0xa509c → 0xa50a0）—— 每个 opcode 的 handler 都归错人（这正是某份取证包犯过的错）',
  },
  // ── 帧机制（★ 塌了会静默：单层调用看起来正常，只在嵌套/多次调用时错开）──
  {
    file: 'apps/emulator/src/vm/machine.ts',
    from: 'return v === undefined ? null : v;',
    to: 'return v === undefined ? 0 : v;',
    guard: 'tools/test/emulator-frames.test.mjs',
    what: '`ret` 空栈返回 0 而不是 `null` ⇒ 空栈变成"返回到第 0 条"（引擎是直接 retn、什么都不做）',
  },
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: 'const next = ctx.frame.ip + 1; // ★ 引擎压的 `序号+3`（3 = call 的 dword 数）换算成下标就是 ip+1',
    to: 'const next = ctx.frame.ip; // MUTANT',
    guard: 'tools/test/emulator-frames.test.mjs',
    what: '`call` 压错返回点（压调用点本身而不是它的下一条）⇒ 每次返回都重执行调用指令 = 死循环',
  },
  // ── 地址空间与随机源（★ 这两样塌了，"指针能解引用"与"同种子同日志"都会变成假象）──
  {
    file: 'apps/emulator/src/model/address-space.ts',
    from: 'return this.base + this.elemBytes * index;',
    to: 'return this.base + this.elemBytes * (index + 1);',
    guard: 'tools/test/emulator-address-space.test.mjs',
    what: '`base + elemBytes*index` 整体偏移一格 —— 数组遍历会读到相邻的池（类型对、值错）',
  },
  {
    file: 'apps/emulator/src/model/address-space.ts',
    from: 'if (snap.baseCursor < end) {',
    to: 'if (false) {',
    guard: 'tools/test/emulator-address-space.test.mjs',
    what: '恢复时不再拒绝"baseCursor 落在已有区域里" ⇒ 恢复后重发地址 = 两块数据别名',
  },
  {
    file: 'apps/emulator/src/host/random.ts',
    from: 'this.state = (this.state + 0x6d2b79f5) >>> 0;',
    to: 'this.state = (this.state + 0x6d2b79f6) >>> 0;',
    guard: 'tools/test/emulator-address-space.test.mjs',
    what: 'PRNG 的推进常数改一位 ⇒ 序列变了（守卫里逐值钉死了种子 1 的前三个数）',
  },
  // ── headless 前端（宿主层 / 操作数 / 场景）★ 这三条守的都是"静默"型坏结果 ──
  {
    file: 'apps/emulator/src/host/fs.ts',
    from: "if (s === '..')",
    to: "if (s === '..\\u0000')",
    guard: 'tools/test/emulator-host.test.mjs',
    what: '名字归一化里的 `..` 拒绝失效 —— 引擎侧的名字就能逃出根（而**不会报错**）',
  },
  {
    file: 'apps/emulator/src/vm/operand.ts',
    from: 'export const POINTER_OPERAND_TYPES: readonly number[] = [0x6, 0x7, 0x8, 0xc, 0xd, 0xe];',
    to: 'export const POINTER_OPERAND_TYPES: readonly number[] = [0x6, 0x7, 0x8, 0xc, 0xd];',
    guard: 'tools/test/emulator-host.test.mjs',
    what: '指针族少一个 type（0xe）—— 于是它会被当普通池读：**类型对、值错，且不报错**',
  },
  {
    file: 'apps/emulator/src/model/scene.ts',
    from: 'if (it) { it.workingArgb = w.window.toArgb >>> 0; it.window = null; }',
    to: 'if (it) { it.workingArgb = w.window.fromArgb >>> 0; it.window = null; }',
    guard: 'tools/test/emulator-host.test.mjs',
    what: '计时窗跑完时把工作色提交成 **FROM**（而不是 TO）—— 画面上是"淡入淡出永远停在起点"',
  },
  {
    file: 'packages/age-format/src/asm/value-codec.mts',
    from: 'rol32(x >>> 0, 11)', to: 'rol32(x >>> 0, 12)',
    guard: 'tools/test/engine-value-codec.test.mjs',
    what: 'DEC 的移位量 11 → 12',
  },
  {
    file: 'packages/age-format/src/asm/value-codec.mts',
    from: 'ror32(v >>> 0, 7)', to: 'ror32(v >>> 0, 8)',
    guard: 'tools/test/engine-value-codec.test.mjs',
    what: 'ENC 的移位量 7 → 8',
  },
  {
    // ★ 这一条是 2026-10 三次订正留下的**回归钉**：记录基址 `0x5D894`（旧口径把它取成 `0x5D880` = `cur` 的槽）。
    //   把基址改回旧值 ⇒ 记录内偏移全错（`0x5D904` 会被记成 `+0x84` 而不是 `+0x70`）⇒ 新判据**当场红**。
    file: 'packages/age-format/src/engine/layout.mts',
    from: 'base: 0x5d894,',
    to: 'base: 0x5d880,',
    guard: 'tools/test/engine-frame.test.mjs',
    what: '记录基址 `0x5D894` 改回 `0x5D880`（= 把 `cur` 那个 Engine 级标量的槽当成记录基址）⇒ 以 cur 索引的字段落点与记录大小判据当场红',
  },
  {
    // ★ 同一条订正的另一半：`stride` 既是索引步长又是**记录大小**（最大字段偏移 `+0x74`，`+4` = 记录边界）。
    file: 'packages/age-format/src/engine/layout.mts',
    from: 'stride: 0x78,', to: 'stride: 0x8c,',
    guard: 'tools/test/engine-frame.test.mjs',
    what: '记录大小/步长 `0x78` 改成 `0x8c`（= 旧口径"至少 0x8C"那个假大小）⇒ "`+0x74 + 4 == stride`"与 40 份记录末尾的边界判据当场红',
  },
  {
    // ★ 回归钉：把 `array_container` 挪到相邻格（`+0x70` → `+0x74`）⇒ 两格混成一格，
    //   而"同一格有扁平与索引两种形态"那条普查判据 + 逐字读点 EA 必须一起红。
    file: 'packages/age-format/src/engine/layout.mts',
    from: '    arrayContainer: 0x70,',
    to: '    arrayContainer: 0x74,',
    guard: 'tools/test/emulator-model.test.mjs',
    what: '`记录+0x70`（array_container，绝对 `0x5D904`）挪到 `+0x74`（两个相邻格混成一格）—— 普查出的读点 EA 与形态必须一起红',
  },
  {
    file: 'packages/age-format/src/engine/layout.mts',
    from: 'stride: 0x78,', to: 'stride: 0x80,',
    guard: 'tools/test/emulator-model.test.mjs',
    what: '记录步长 0x78 → 0x80（★ 变量在布局知识层，不在模拟器里）',
  },
  // ── 帧数（★ 改坏了 ⇒ 帧区末尾与"每个 per-cur 记录占 0x78 字节"这条已被守卫钉住的判据一起红）──
  {
    file: 'packages/age-format/src/engine/layout.mts',
    from: '  count: 40,', to: '  count: 39,',
    guard: 'tools/test/emulator-model.test.mjs',
    what: '记录数 40 → 39（记录区末尾 0x5EB54 与"记录大小 = 索引步长 = 0x78"的算术判据必须一起红）',
  },
  {
    // ★ 这一条是 2026-10 三次订正留下的**回归钉**：`float` 的基址在记录 `+0x24`（绝对 `0x5D8B8`），
    //   而守卫按"取址原语 case 10 读哪个地址"判 ⇒ 值错、或把它挪到 `array_container`(`+0x70`) 都会红。
    file: 'packages/age-format/src/engine/layout.mts',
    from: "{ name: 'float', count: 0x0c, base: 0x24 }",
    to: "{ name: 'float', count: 0x0c, base: 0x70 }",
    guard: 'tools/test/emulator-model.test.mjs',
    what: 'local_float 基址 `+0x24` → `+0x70`（把 array_container 当成池基址；两次订正都栽在"看成对的偏移"上）',
  },
  {
    // ★ 另一条回归钉：把两个池的**偏移对调**（集合不变、配对错）—— 只有"按 case 号配 type"的判据能抓它
    file: 'packages/age-format/src/engine/layout.mts',
    from: "{ name: 'int', count: 0x08, base: 0x20 },",
    to: "{ name: 'int', count: 0x08, base: 0x24 },",
    guard: 'tools/test/emulator-model.test.mjs',
    what: '把 int 的基址 `+0x20` 改成 `+0x24`（与 float 撞车）⇒ "6 个基址的集合"少了 `0x5D8B4`、且 case 9 读的不是它 ⇒ 配对判据必须红',
  },
  {
    // ★ 脚本头那 6 个 local 声明：**每项 4 字节、整个 u32 就是一个 count**，声明 ↔ 池是**位置对应**。
    //   把前两池的 count 槽对调 ⇒ 语料侧的判据（6 对「计数源 → 记录+0x08+4i」+ 32 字节定长读）必须当场红；
    //   同时 `types.mts` 的 `FIELD_OFFSETS` 那条变异覆盖"文件字节 8/12/16/20/24/28"这一半。
    file: 'packages/age-format/src/engine/layout.mts',
    from: "{ name: 'int', count: 0x08, base: 0x20 },\n  { name: 'float', count: 0x0c, base: 0x24 }",
    to: "{ name: 'int', count: 0x0c, base: 0x20 },\n  { name: 'float', count: 0x08, base: 0x24 }",
    guard: 'tools/test/emulator-model.test.mjs',
    what: '脚本头两个 local 声明的 count 槽对调（`int` `+0x08` ↔ `float` `+0x0C`）—— "位置对应"这条口径被破坏，而只按字面表自证的断言看不出来',
  },
  {
    // ★ 头部 13 个 u32 字段的**顺序与相对偏移**：前 6 项 = local 声明区（读窗口字节 8..28）。
    //   把第 0 项从 8 挪到 12 ⇒ 第 0 个声明落到别的字节上 ⇒ pure 用例当场红。
    file: 'packages/age-format/src/asm/types.mts',
    from: 'export const FIELD_OFFSETS = [8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56];',
    to: 'export const FIELD_OFFSETS = [12, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56];',
    guard: 'packages/age-format/test/asm.test.mjs',
    what: '`FIELD_OFFSETS[0]` 8 → 12（第 0 个 local 声明落到别的文件字节上）—— 前 6 项必须是从字节 8 起的连续 u32',
  },
  {
    // ── codec key：`CODEC.keyField.dword` 是"key 在 Engine 里的 dword 索引"（97059 ⇒ 0x5EC8C）。
    //   ★ 加这条之前，把它改成别的数**无声通过**（`dec.keyLoad`/`enc.keyLoad` 是写死的字面串，
    //     与 `keyField.dword` 脱钩）⇒ G1 把两者**钉在一起**（唯一写入点 EA + dword×4 == 0x5EC8C）。
    file: 'packages/age-format/src/asm/value-codec.mts',
    from: "keyField: { name: 'Engine+0x5EC8C', dword: 97059 },",
    to: "keyField: { name: 'Engine+0x5EC8C', dword: 97060 },",
    guard: 'tools/test/engine-value-codec.test.mjs',
    what: 'codec key 的槽号 97059 → 97060（0x5EC8C → 0x5EC90 = enc_zero 那一格）—— 会把"key"与"enc_zero"静默混成一格',
  },
  {
    file: 'apps/emulator/src/model/iterate.ts',
    from: 'export const instrByteLength = (argc: number): number => 4 + 8 * (argc >>> 0);',
    to: 'export const instrByteLength = (argc: number): number => 8 + 8 * (argc >>> 0);',
    guard: 'tools/test/emulator-iterate.test.mjs',
    what: '指令字节长度 4+8*argc → 8+8*argc',
  },
  {
    file: 'apps/emulator/src/model/numeric-ops.ts',
    // ★ 字面量随**语料来源订正**改过（2026-10 需求单 `REQ-01M4G9YTEKGER7N99C3M7F443R`）：
    //   `0x2D2` 在**原始语料**里出现 **1** 次（发行树那一份才是 0）。下面那条是"抹成 0"的反方向破坏。
    from: '{ opcode: 0x2d2, name: \'\', argc: 3, staticUses: 1,',
    to: '{ opcode: 0x2d2, name: \'\', argc: 3, staticUses: 7,',
    guard: 'tools/test/emulator-numeric-ops.test.mjs',
    what: '0x2D2 的 staticUses 1 → 7（「语料里出现几次」那条判据；★ 语料来源已改成清单 `gameInstall` + ALF ⇒ 这条在本机真的会跑）',
  },
  {
    file: 'apps/emulator/src/model/numeric-ops.ts',
    from: "{ opcode: 0x50, name: 'add', argc: 3,",
    to: "{ opcode: 0x50, name: 'add', argc: 2,",
    guard: 'tools/test/emulator-numeric-ops.test.mjs',
    what: 'add 的 argc 3 → 2（argc 错 ⇒ 整条流错位）',
  },
  {
    file: 'apps/emulator/src/model/pools.ts',
    from: 'return [...m.entries()].sort((a, b) => a[0] - b[0]);',
    to: 'return [...m.entries()];',
    guard: 'tools/test/emulator-snapshot.test.mjs',
    what: '快照不再按键升序 ⇒ 同样状态给出不同字节（`Map` 的迭代顺序 = 插入顺序）',
  },
  {
    file: 'apps/emulator/src/model/pools.ts',
    // ★ 钉**最稳定的子串**（`oob: 'diagnostic'`）：整行会随"新增字段"变，子串不会
    from: "oob: 'diagnostic',",
    to: "oob: 'engine',",
    guard: 'tools/test/emulator-state-partition.test.mjs',
    what: '把 `oob` 从 diagnostic 改成 engine（诊断被当成引擎态 ⇒ 快照会多带一个会涨的量）',
  },
  {
    file: 'tools/lib/disasm.mjs',
    from: "const PROC_EA_RE = new RegExp(`^([A-Za-z_.][\\\\w.]*):([0-9A-Fa-f]{8})\\\\s+(${SYM})\\\\s+proc near`);",
    to: "const PROC_EA_RE = new RegExp(`^([A-Za-z_][\\\\w.]*):([0-9A-Fa-f]{8})\\\\s+(${SYM})\\\\s+proc near`);",
    guard: 'tools/test/disasm-pseudo.assets.test.mjs',
    what: '段名首字符不许 `.` ⇒ `.text` 的 proc near 全漏（这个坑在本仓踩过两次）',
  },
  // ── 符号名不许含 `@`（MSVC 修饰名）⇒ `_WinMain@16` 不被认作函数头，`enclosingFunction` **静默**给前一个函数 ──
  {
    file: 'tools/lib/disasm.mjs',
    from: "const SYM = '[A-Za-z_$?@][\\\\w.$?@]{0,60}';",
    to: "const SYM = '[A-Za-z_][\\\\w.]{0,60}';",
    guard: 'tools/test/disasm-pseudo.assets.test.mjs',
    what: '符号名不许含 `@` ⇒ `_WinMain@16 proc near` 不被认作函数头，`--pseudo --ea 0x4BAA3A` 静默报出**前一个**函数（`sub_4BA6B0`）',
  },
  // ── 需求台账：写路径"报的落盘"必须与磁盘一致 ──
  {
    file: 'tools/requirements.mjs',
    from: '  if (res?.rollback) {',
    to: '  if (false) {',
    guard: 'tools/test/requirements-add-lands.test.mjs',
    what: '被写后守卫拒回时不再拒绝（继续按"已落盘"打印计划）⇒ stdout 报成功而盘上什么都没有',
  },
  // ── 发布链：AGF 缺省来源必须还是"入库件"（★ 别退回"每次重烧"）──
  {
    file: 'tools/lib/release.mjs',
    from: "    agfDir: opts.baked ?? opts.agfDir ?? DEFAULT_AGF_DIR,",
    to: '    agfDir: opts.baked ?? opts.agfDir ?? DEFAULT_BAKED_DIR,',
    guard: 'tools/test/release-agf-source.test.mjs',
    what: '发布链的 AGF 缺省来源退回 `dist/ui-bake`（每次重烧）⇒ 又要 headless Chrome，且发出去的字节变成"这次烧出来的"',
  },
  // ── 平台路径的缺省值必须来自清单（★ 硬编码回去了 ⇒ 换机器就"找不到原始件"）──
  {
    file: 'tools/lib/ui-bake/bake.mjs',
    from: '  if (!roots.gameInstall) throw new Error(',
    to: '  if (false) throw new Error(',
    guard: 'tools/test/manifest-local.test.mjs',
    what: '清单里缺 `roots.gameInstall` 时不再响亮失败 ⇒ 退回 `undefined`，换机器时表现为"素材没到位"而不是"清单该覆盖"',
  },
  // ── 函数覆盖度：库代码分类（★ 不分出来 ⇒ "下一步该取证谁"会指向 CRT/STL 的库代码）──
  {
    file: 'tools/lib/coverage.mjs',
    from: "  if (f.callees.length === 0 && !f.hasMemoryOperand && f.externalTargets.length > 0) reasons.push('thin-forwarder-to-library');",
    to: '  // (mutated: 不再判"只往库里转一手")',
    guard: 'tools/test/ledger-coverage.test.mjs',
    what: '库代码分类失效（/GS 桩那类"只往库里转一手"的函数会被算进引擎宇宙）⇒ 分桶与前沿都会指向库代码',
  },
  // ── 库候选的**收窄**（★ 阈值一放宽 ⇒ `sub_42B4B0` 这类引擎原语又会被摘出前沿榜与分桶）──
  {
    file: 'tools/lib/coverage.mjs',
    from: 'export const ENGINE_SCALE_DISP = 0x1000;',
    to: 'export const ENGINE_SCALE_DISP = 0x100000;',
    guard: 'tools/test/ledger-coverage.test.mjs',
    what: '库候选收窄的阈值 0x1000 → 0x100000 ⇒「内联 STL 抛出口 + Engine 字段位移」的引擎原语（`sub_42B4B0`/`sub_42BA00`）又被判成库候选，整条从两张前沿榜与分桶里消失（真语料那条判据另有 `ledger-coverage.assets.test.mjs`）',
  },
  // ── 函数覆盖度：`complete` 的不动点（★ 不删"callee 没登记"的成员 ⇒ 所有 observed 都被算成收口）──
  {
    file: 'tools/lib/coverage.mjs',
    from: '      const bad = (f?.callees ?? []).some((c) => !complete.has(c));',
    to: '      const bad = false;',
    guard: 'tools/test/ledger-coverage.test.mjs',
    what: '覆盖度的不动点不再剔除"callee 未登记"的成员 ⇒ `partial-rooted` 桶清零、把没收口的函数说成收口（"下一步该取证谁"跟着错）',
  },
  // ── 函数覆盖度：锚 EA 归属的上界（★ 去掉"最后一条 EA"这一界 ⇒ 段尾/`.data` 的地址被算进最后一个函数）──
  {
    file: 'tools/lib/disasm.mjs',
    from: '  if (ea > f.lastEa) return null;',
    to: '  if (false) return null;',
    guard: 'tools/test/ledger-coverage.test.mjs',
    what: 'EA 归属只看"下一个函数的起点" ⇒ 函数末尾之后的空隙（padding / 段尾）被错误归属给前一个函数',
  },
  // ── 本机私有清单覆盖（平台相关路径）──
  {
    file: 'tools/lib/manifest.mjs',
    from: '  if (local) manifest.roots = { ...(manifest.roots ?? {}), ...local.roots };',
    to: '  if (local) void local;',
    guard: 'tools/test/manifest-local.test.mjs',
    what: '本机私有覆盖被静默忽略 ⇒ 换一台机器（win32 ↔ macOS）所有来源根都指向另一台机器的路径',
  },
  // ── 头部三组 (长度, 偏移) 的**配对**（★ 把 `table_1_offset` 与 `table_2_length` 的槽抄错一格 ⇒ 表读到别处，而"读到一张表"看起来正常）──
  {
    file: 'packages/age-format/src/asm/types.mts',
    from: 'export const FIELD_OFFSETS = [8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56];',
    to: 'export const FIELD_OFFSETS = [8, 12, 16, 20, 24, 28, 32, 36, 44, 40, 48, 52, 56];',
    guard: 'tools/test/emulator-frame-tables.assets.test.mjs',
    what: '头部 `table_1_offset` ↔ `table_2_length` 的槽互换（40 ↔ 44）⇒ 三张表读到别处：`headerLen + 4*表项` 处不再是该类 opcode（"表项 = 位置"这条读数失效）',
  },
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: 'const jumped = cond !== 0 ? jumpToLabel(ctx, 1) : jumpToLabel(ctx, 2);',
    to: 'const jumped = cond === 0 ? jumpToLabel(ctx, 1) : jumpToLabel(ctx, 2);',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`0xa0 jcc` 的两支**对调**（条件非 0 跳 op3）—— 每一条 `if` 都走反，而"程序还在跑"',
  },
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: 'maskDiscarded: true, latchCleared: true,',
    to: 'maskDiscarded: false, latchCleared: true,',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '`0x101 poll-input` 的"采样被丢弃"不再成立（写反成"这次采样留下"）—— 脚本里"等一次点击"的循环会**看起来正常地空转**',
  },
  {
    file: 'apps/emulator/src/model/numeric-ops.ts',
    // ★ 与上面那条 `staticUses: 1 → 7` **同值、不同破坏**：`0x2D2` 在新语料里出现 **1** 次
    //   ⇒ "把出现次数抹成 0"也是一种破坏（它与"零出现集合"那条断言直接冲突）。
    //   ⛔ 别再写成迁移前的 `0 → 7`：那个字面量已随语料来源订正消失（见 numeric-ops.ts 文件头）。
    from: '{ opcode: 0x2d2, name: \'\', argc: 3, staticUses: 1,',
    to: '{ opcode: 0x2d2, name: \'\', argc: 3, staticUses: 0,',
    guard: 'tools/test/emulator-numeric-ops.test.mjs',
    what: '`0x2D2` 的 staticUses 1 → 0（把"原始语料里出现过"抹成"零出现" ⇒ 零出现集合那条断言当场红）',
  },
  {
    // ★ 语料锚（`emulator-engine-scalars.assets.test.mjs`）：它拿 `CTOR_ZERO_FILLED` 去 `.lst` 里
    //   **逐字**对那 20 格。纯用例那条断言 `?? 1` 是**另一件事**（模型行为），这条守的是**取证本身**。
    file: 'apps/emulator/src/model/engine-scalars.ts',
    from: "Array.from({ length: 20 }, (_, i) => 0x5ec9c + 4 * i),",
    to: "Array.from({ length: 20 }, (_, i) => 0x5ec9c + 4 * i).slice(0, 19),",
    guard: 'tools/test/emulator-engine-scalars.assets.test.mjs',
    what: '★ 语料锚版：清零格数 20 → 19（那片 `Engine+0x5EC9C..0x5ECE8` 少一格）⇒ `.lst` 里那 20 条逐字对不上',
  },
];

/**
 * 入口（派发器 `tools/cli.mjs` 与直接运行都走这里）。
 * @returns {number} 退出码：0 = 全部按预期变红；1 = 有守卫没抓住破坏；2 = 还原失败（已中止）
 */
export function main(argv = process.argv.slice(2)) {
  // ★ `--help` 必须**只打用法就走**：它原先落到"跑全套变异"那条路（实测踩过一次）——
  //   而本工具**会临时改写工作树里的文件**，在有别的进程（人或 agent）正在编辑时跑，
  //   还原那一步可能把别人的改动一起写回去。
  if (argv.includes('--help') || argv.includes('-h')) {
    console.log(
      '用法：node tools/mutate-check.mjs [--list]\n\n' +
        '  不带参数 = 逐条施加变异 ⇒ 跑对应守卫 ⇒ 要求退出码非 0（最后逐条还原并比对全文）。\n' +
        '  ⛔ **它会临时改写工作树里的文件** ⇒ 有别的进程正在编辑本仓时**不要**跑（还原会覆盖别人的改动）。\n' +
        '  --list 只列清单（不动任何文件）。',
    );
    return 0;
  }
  if (argv.includes('--list')) {
    for (const m of MUTATIONS) console.log(`${m.what}\n    ${m.file}  ⇒ 守卫 ${m.guard}`);
    return 0;
  }

  const results = [];
  for (const m of MUTATIONS) {
    const abs = path.join(ROOT, m.file);
    const before = fs.readFileSync(abs, 'utf8');
    if (!before.includes(m.from)) {
      results.push({ ...m, verdict: '⚠ 变异点没找到（源文件已变？先更新清单）', ok: false });
      continue;
    }
    fs.writeFileSync(abs, before.replace(m.from, m.to));
    let code = null;
    let ran = null; // {pass, fail, skipped, skipReason}
    const log = path.join(os.tmpdir(), `amayui-mutate-${process.pid}.log`);
    try {
      // ★ 沙箱里不能**捕获**子进程输出（管道要命名管道 ⇒ EPERM）⇒ 用**文件描述符重定向**拿同一份答案
      //   （`tools/corpus.mjs` 的 `runCapture()` 就是这么绕的）。为什么要这份输出：
      //   **守卫 skip 时退出码也是 0** —— 只看退出码会把"这台机器上没跑"误报成"守卫没红"（实测踩过：
      //   `emulator-numeric-ops` 的两条要 `dist/install`，本机没有 ⇒ 变异看起来"没抓住"，其实是没跑）。
      const fd = fs.openSync(log, 'w');
      try {
        const r = spawnSync(process.execPath, ['--test', '--test-isolation=none', m.guard], { cwd: ROOT, stdio: ['ignore', fd, fd] });
        code = r.status;
      } finally {
        fs.closeSync(fd);
      }
      const text = fs.readFileSync(log, 'utf8');
      const num = (k) => Number(new RegExp(`^ℹ ${k} (\\d+)$`, 'm').exec(text)?.[1] ?? 0);
      const skipReason = /^﹣ .*?# (.+)$/m.exec(text)?.[1] ?? null;
      ran = { pass: num('pass'), fail: num('fail'), skipped: num('skipped'), skipReason };
    } finally {
      fs.rmSync(log, { force: true });
      // ★ 无论成败都还原；还原后**比对全文**（只比长度不够）
      fs.writeFileSync(abs, before);
    }
    if (fs.readFileSync(abs, 'utf8') !== before) {
      console.error(`\n✗ **还原失败**：${m.file} 与施加前不一致 —— 已中止（工作树可能被污染，请查 git diff）`);
      return 2;
    }
    const skipped = ran && ran.fail === 0 && ran.skipped > 0;
    results.push({
      ...m,
      code,
      ran,
      verdict: code !== 0
        ? `✅ 守卫红了（退出码 ${code}）`
        : skipped
          ? `⏭ **无法判定**（守卫 skip，退出码 0）：${ran.skipReason ?? '（无原因）'}`
          : `❌ **守卫没红**（退出码 0${ran ? `，跑了 ${ran.pass} 条` : ''}）`,
      ok: code !== 0,
      unverifiable: Boolean(skipped),
    });
  }

  console.log('守卫自检（改坏一处常量 ⇒ 指定守卫是否当场红）：\n');
  for (const r of results) console.log(`  ${r.verdict}  ${r.what}`);
  const failed = results.filter((r) => !r.ok && !r.unverifiable);
  const unver = results.filter((r) => r.unverifiable);
  console.log(`\n${results.length - failed.length - unver.length}/${results.length} 条按预期变红${unver.length ? ` · ${unver.length} 条**无法判定**（守卫在本机 skip ⇒ 不能算"没红"）` : ''}。`);
  if (unver.length) {
    console.log('⏭ 这些守卫在**本机**跑不起来（不是"没抓住"）—— 要它有意义就得先备齐它的语料/资产：');
    for (const u of unver) console.log(`  - ${u.what}（${u.guard}）：${u.ran.skipReason ?? '（无原因）'}`);
  }
  if (failed.length) {
    console.error('★ 这些守卫**没抓住**已知的破坏 ⇒ 它们不是"会红的守卫"（断言恒真？或压根没验那个常量）：');
    for (const f of failed) console.error(`  - ${f.what}（${f.guard}）`);
    return 1;
  }
  return 0;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) process.exitCode = main();

