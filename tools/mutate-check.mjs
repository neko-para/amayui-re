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
    from: 'this.baseCursor = base + REGION_STRIDE;',
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
  {
    file: 'apps/emulator/src/model/engine-scalars.ts',
    from: 'return this.values.get(name) ?? 0;',
    to: 'return this.values.get(name) ?? 1;',
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '没写过的标量槽返回 1 而不是 0 —— 引擎把这片清零了（取证在 layout.mts 的 EVIDENCE）',
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
    file: 'packages/age-format/src/engine/layout.mts',
    from: 'stride: 0x78,', to: 'stride: 0x80,',
    guard: 'tools/test/emulator-model.test.mjs',
    what: '帧步长 0x78 → 0x80（★ 变量在布局知识层，不在模拟器里）',
  },
  {
    file: 'packages/age-format/src/engine/layout.mts',
    from: "{ name: 'float', count: 0x20, base: 0x44 }",
    to: "{ name: 'float', count: 0x20, base: 0x84 }",
    guard: 'tools/test/emulator-model.test.mjs',
    what: 'local_float 基址 0x44 → 0x84（★ 这正是我批 R1 犯过的错：把 array_container 当成池基址）',
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
    from: '{ opcode: 0x2d2, name: \'\', argc: 3, staticUses: 0,',
    to: '{ opcode: 0x2d2, name: \'\', argc: 3, staticUses: 7,',
    guard: 'tools/test/emulator-numeric-ops.test.mjs',
    what: '0x2D2 的 staticUses 0 → 7（「语料里零出现」那条判据）',
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
    from: "const PROC_EA_RE = /^([A-Za-z_.][\\w.]*):([0-9A-Fa-f]{8})\\s+([A-Za-z_][\\w.]{0,40})\\s+proc near/;",
    to: "const PROC_EA_RE = /^([A-Za-z_][\\w.]*):([0-9A-Fa-f]{8})\\s+([A-Za-z_][\\w.]{0,40})\\s+proc near/;",
    guard: 'tools/test/disasm-pseudo.assets.test.mjs',
    what: '段名首字符不许 `.` ⇒ `.text` 的 proc near 全漏（这个坑在本仓踩过两次）',
  },
];

/**
 * 入口（派发器 `tools/cli.mjs` 与直接运行都走这里）。
 * @returns {number} 退出码：0 = 全部按预期变红；1 = 有守卫没抓住破坏；2 = 还原失败（已中止）
 */
export function main(argv = process.argv.slice(2)) {
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
    try {
      // ★ 沙箱里不能捕获子进程输出（管道要命名管道 ⇒ EPERM）⇒ 只看**退出码**
      const r = spawnSync(process.execPath, ['--test', '--test-isolation=none', m.guard], { cwd: ROOT, stdio: 'ignore' });
      code = r.status;
    } finally {
      // ★ 无论成败都还原；还原后**比对全文**（只比长度不够）
      fs.writeFileSync(abs, before);
    }
    if (fs.readFileSync(abs, 'utf8') !== before) {
      console.error(`\n✗ **还原失败**：${m.file} 与施加前不一致 —— 已中止（工作树可能被污染，请查 git diff）`);
      return 2;
    }
    results.push({ ...m, code, verdict: code !== 0 ? `✅ 守卫红了（退出码 ${code}）` : '❌ **守卫没红**（退出码 0）', ok: code !== 0 });
  }

  console.log('守卫自检（改坏一处常量 ⇒ 指定守卫是否当场红）：\n');
  for (const r of results) console.log(`  ${r.verdict}  ${r.what}`);
  const failed = results.filter((r) => !r.ok);
  console.log(`\n${results.length - failed.length}/${results.length} 条按预期变红。`);
  if (failed.length) {
    console.error('★ 这些守卫**没抓住**已知的破坏 ⇒ 它们不是"会红的守卫"（断言恒真？或压根没验那个常量）：');
    for (const f of failed) console.error(`  - ${f.what}（${f.guard}）`);
    return 1;
  }
  return 0;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) process.exitCode = main();

