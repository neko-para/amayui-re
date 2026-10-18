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
  // ── 语料 item 目录：**定义 vs 原型**的判据（★ 旧判据只看"行首 + 名字像函数 + 行尾 `;`"，
  //    于是**带尾注释的原型**（`…); // idb`）被当成定义，该符号的"C 体"是**文件顶部的一行原型**，
  //    调用方照样打印、不报错 —— 实测 40 个符号）──
  //   ★ 这条变异必须**同时**拿掉两个判据（`;` 收尾 与 "下一个非空行恰为 `{`"）：只拿掉一个，
  //     合成样本里的单行原型仍会被另一个挡住 ⇒ 守卫不会红，而那**不是**"守卫没用"。
  {
    file: 'tools/lib/catalog.mjs',
    from:
      "  if (text.slice(text.lastIndexOf(')') + 1).trim().startsWith(';')) return null; // 原型\n" +
      '  let j = k + 1;\n' +
      "  while (j < lines.length && lines[j].trim() === '') j += 1;\n" +
      "  if (lines[j] !== '{') return null; // 不是\"定义 + 紧跟函数体\"",
    to:
      '  if (false) return null; // 变异：原型与定义都不再被排除\n' +
      '  let j = k + 1;\n' +
      "  while (j < lines.length && lines[j].trim() === '') j += 1;",
    guard: 'tools/test/disasm-fn-format.test.mjs',
    what: '原型不再被排除 ⇒ 带尾注释的原型被当成定义，C 行区间落进原型区（"一行 C 体"看起来完全正常）',
  },
  // ── 语料 item 目录：**参数配对**是"名字优先、顺序兜底"（★ 只按位置会给出错的偏移，实测 sub_401100）──
  {
    file: 'tools/lib/catalog.mjs',
    from: '&& o.name === p.name);',
    to: '&& false);',
    guard: 'tools/test/disasm-fn-format.test.mjs',
    what: '去掉"名字优先"这一步 ⇒ 两边逐字同名的 `lpFileName` 被按位置配到 0Ch，参数偏移全错而**看不出来**',
  },
  // ── 语料 item 目录：数据长度要算**字符串的字符数**（★ 只数元素个数 ⇒ `.lst` 给 2、`.c` 写 `char aError[6]`）──
  {
    file: 'tools/lib/catalog.mjs',
    from: 'return parts.length + strBytes - strSlots;',
    to: 'return parts.length;',
    guard: 'tools/test/disasm-fn-format.test.mjs',
    what: '数据 item 的长度只数"元素个数" ⇒ 字符串只算 1 字节（`char aError[6]` 与 `.lst` 侧的 6 字节对不上）',
  },
  // ── 格式词汇表：间接调用**必须认 `ds:`**（★ 旧正则漏掉这一整类，实测 69 处被静默丢出调用图）──
  {
    file: 'tools/lib/lst-format.mjs',
    from: '|ds:|es:|ss:|cs:)/;',
    to: ')/;',
    guard: 'tools/test/disasm-fn-format.test.mjs',
    what: '`INDIRECT_CALL_RE` 去掉 `ds:` ⇒ `call ds:dword_X` 这一整类调用点**既不进直接边、也不计间接**（只是数字变小）',
  },
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
  // ── ★★ 配置键里那个数：**值** vs **序号**（这一条守的是"近似不许被悄悄换成引擎口径"）──
  //   引擎逐字是**序号**（`sub_42DF40` 的 `0x42DF6F push 1` + `call sub_418A30` 返回表下标），
  //   而本层实现用的是**操作数的值** —— 换成序号会**真的推进启动链**（SYSTEM4 的 `load-int (global-int 5)`
  //   紧挨 `jcc (global-int 5)`）⇒ 按用户限制不在本轮。所以这里要一个**会红**的钉子：
  //   一旦有人把 `configKeyIntByValue` 换成序号口径，指定用例必须当场红（而不是靠读注释发现）。
  {
    file: 'apps/emulator/src/vm/ops.ts',
    from: "return `\\u0003${(value >>> 0).toString(16).padStart(8, '0')}`;",
    to: "return `\\u0003${(1).toString(16).padStart(8, '0')}`;",
    guard: 'tools/test/emulator-engine-scalars.test.mjs',
    what: '★ 配置键从"操作数的**值**"变成"操作数**序号**"（= 引擎口径）⇒ "本层近似"那条断言必须当场红（否则这个近似会在没人注意的时候被换掉，且启动链分叉随之改变）',
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
    file: 'tools/lib/lst-format.mjs',
    from: "export const PROC_EA_RE = new RegExp(`^([A-Za-z_.][\\\\w.]*):([0-9A-Fa-f]{8})\\\\s+(${SYM})\\\\s+proc near`);",
    to: "export const PROC_EA_RE = new RegExp(`^([A-Za-z_][\\\\w.]*):([0-9A-Fa-f]{8})\\\\s+(${SYM})\\\\s+proc near`);",
    guard: 'tools/test/disasm-enclosing.assets.test.mjs',
    what: '段名首字符不许 `.` ⇒ `.text` 的 proc near 全漏（这个坑在本仓踩过两次）',
  },
  // ── 符号名不许含 `@`（MSVC 修饰名）⇒ `_WinMain@16` 不被认作函数头，`enclosingFunction` **静默**给前一个函数 ──
  {
    file: 'tools/lib/lst-format.mjs',
    from: "export const SYM = '[A-Za-z_$?@][\\\\w.$?@]{0,60}';",
    to: "export const SYM = '[A-Za-z_][\\\\w.]{0,60}';",
    guard: 'tools/test/disasm-enclosing.assets.test.mjs',
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
    file: 'tools/lib/catalog.mjs',
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
  // ── 清单写入口的闸门：「不新增失败」（★ 改回"零失败"或让差集恒空 ⇒ 缺件机器上写不进去 / 什么都能写进去）──
  {
    file: 'tools/lib/manifest.mjs',
    from: '  return failureKeys(afterReport).filter((k) => !before.has(k));',
    to: '  return [];',
    guard: 'tools/test/corpus-manifest.test.mjs',
    what: '写入口的差集恒空 ⇒ **新写入自己引出来的失败也不再挡**（清单变成"写什么是什么"；`REQ-01M52F9K25YNPJEXQ74QE4CT9Z` 的判据 1 另一半）',
  },
  // ── origin 的 machine-local 档（★ 抹掉这一档 ⇒ 本机现场产物又被判红，缺件机器回到"什么都写不了"）──
  {
    file: 'tools/lib/manifest.mjs',
    from: "export const isMachineLocal = (o) => o?.disposition === 'machine-local';",
    to: 'export const isMachineLocal = () => false;',
    guard: 'tools/test/corpus-manifest.test.mjs',
    what: 'machine-local 档整条失效（#3 缺件重新硬红、#4 又要求 pin、`--scan` 又给本机产物补 pin）⇒ 缺件机器上 `corpus validate` 长期红且写不进去',
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
  // ── 台账锚点的 PE 映射（★ 偏移整体少 0x1000 ⇒ 观察静默落到**前一个函数的体内**，而"读到字节"看起来正常）──
  {
    file: 'tools/lib/ledger.mjs',
    from: '      return { rva, imageBase, section: name, offset: rawPtr + (rva - va) };',
    to: '      return { rva, imageBase, section: name, offset: rawPtr + (rva - va) - 0x1000 };',
    guard: 'tools/test/ledger.pe.external.test.mjs',
    what: 'EA → 文件偏移整体少一页 0x1000（= 那次误判里"工具打 0x95a20"那个数）⇒ 段内函数起点的字节不再是 `.lst` 里那条指令的编码',
  },
  // ── 台账摘要棘轮（★ 模拟"只给 len 就算齐"⇒ 缺摘要的观察可以静默进台账，而 sha256 是唯一证据）──
  {
    file: 'tools/lib/ledger.mjs',
    from: '      if (a.len === undefined || a.sha256 === undefined) {',
    to: '      if (a.len === undefined) {',
    guard: 'tools/test/ledger-reference-bin-digest.test.mjs',
    what: '缺摘要的判据从"`len` 或 `sha256` 缺一即算缺"缩成"只缺 `len` 才算缺" ⇒ 只给 `len` 的 `reference` `bin` 锚被当成齐的（写路径与门禁同时失守：换了二进制也验不出来）',
  },
  // ── 台账 `bin` 锚的**站点口径**（★ 去掉"落指令中间必须在 allowlist 里"这条判据 ⇒ 锚 EA 与 claim 引用地址
  //   错位（指令中间 / 0x1000 级）整类静默通过 —— 而"EA 能映射成偏移、字节也读得到"看起来一切正常）──
  {
    file: 'tools/lib/anchor-lst.mjs',
    from: "      if (c.cls === 'B') {",
    to: '      if (false) {',
    guard: 'tools/test/ledger.anchor-lst.assets.test.mjs',
    what: '★ 站点口径：去掉"落在指令中间的锚必须在 AUDIT_ALLOWLIST 里"这条判据 ⇒ 落指令中间的锚不再报红（锚点整类绿而错）',
  },
  // ── shape 域（`tools/shape.mjs` + `tools/lib/shape-*.mjs`）：段落划分判据与两道守卫 ──
  {
    file: 'tools/lib/shape-regions.mjs',
    from: "const CALLER_SAVED = new Set(['eax', 'ecx', 'edx']);",
    to: "const CALLER_SAVED = new Set(['ecx', 'edx']);",
    guard: 'tools/test/shape-regions.test.mjs',
    what: '★ caller-saved 集合删掉 `eax` ⇒ "段落活着的窗口里被 `call` 碰过"这条判据对 `eax` 失效（值随时可能被抹掉，却仍被当成连续推进）⇒ 纯测试当场红',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: "export const VOCAB = ['opcode', 'argc', 'operand', 'lengthWord', 'op1', 'op2', '操作数', '长度字'];",
    to: "export const VOCAB = ['opcode', 'operand', 'lengthWord', 'op1', 'op2', '操作数', '长度字'];",
    guard: 'tools/test/shape-regions.test.mjs',
    what: '词汇守卫词表去掉 `argc` ⇒ "业务概念混进汇编层"整类不再报红（引擎层的东西会静默长进 claims 文件）⇒ 纯测试当场红',
  },
  // ── shape 域（第二组）：**类型真源 `typesFrom` 的守卫覆盖** 与 **站点分桶守恒** ──
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '  for (const w of vocabViolations(M)) red(',
    to: '  for (const w of vocabViolations(model0)) red(',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 两道守卫退回"只查原始 model0" ⇒ `typesFrom` 引用的类型文件里塞 `argc` / `mn` 时**两道守卫一起静默失效**（类型集中在 types.json 之后，那等于守卫只守族文件、不守类型表）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: "    const symbolSites = sites.filter((s) => s.baseKind === 'absolute');",
    to: '    const symbolSites = [];',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 守恒③ 的"数据段符号"桶被清空 ⇒ `ds:` 符号站点从访问总数里**静默消失**（表现是数字变小，不是报错）⇒ 纯测试当场红',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '      if (seen.has(m.offN)) continue;',
    to: '      continue;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G7 不再过滤"已观察到解引用"的 `kind:"ptr"` 成员 ⇒ 每条指针声明都被报成"空话声明"（噪声淹掉真问题：该报的那条再也挑不出来）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: "  if (s.problems?.includes('index')) return 'index-unresolved';",
    to: '  if (false) return \'index-unresolved\';',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 索引未解析的访问重新被当成"偏移观察" ⇒ 位移少了索引那一项却被当成偏移 0/N（轻则假残差，重则**假解释**：正好命中某个成员）',
  },
  {
    file: 'tools/lib/shape-census.mjs',
    from: '        const o = offsets.get(key);',
    to: '        const o = { abs: key, sites: 0, handlers: new Set(), eas: [] };',
    guard: 'tools/test/shape-census.assets.test.mjs',
    what: '★ 全表普查的偏移聚类不再落账 ⇒ `Σ 偏移簇站点 ≠ obj0 站点`（"哪些偏移被碰、被谁碰"整类静默消失）',
  },
  // ── shape 域（第三组）：**声明的视图（推测）** 与 **条数未观察的数组** ──
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '  if (s.absDisp < w.from || s.absDisp >= w.to) return false;',
    to: '  if (false) return false;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 窗口**范围**判据被拆掉（`count:null` 的成员本可无界）⇒ 窗口外的偏移被"吞掉"成**假解释**（实测形态：`0x5ee04` 一路吞到 `0xa609c`）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '        if (!sw || sw.stride == null || !sw.index) {',
    to: '        if (false) {',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ `count:null`（条数未观察）不再要求 `sweep` ⇒ "形态不可判定"的成员照样被当成数组解释（声明不可判定却不报）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: 'explained.push({ ea: s.ea, abs: s.absDisp, member: host, leaf, delta: w.delta, viaView: w.delta !== 0, viaEmbedded });',
    to: 'explained.push({ ea: s.ea, abs: s.absDisp, member: host, leaf, delta: w.delta, viaView: false, viaEmbedded });',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 视图解释不再标成**推断** ⇒ 观测档与推测档混在一起（"哪条结论是模型猜的"看不出，守恒④ 也就名存实亡）',
  },
  // ── shape 域（第四组）：**对象身份的路径**（只管一跳）与**局部类型载体** ──
  {
    file: 'tools/lib/shape-access.mjs',
    from: '      pointeePath: path,',
    to: '      pointeePath: [],',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 站点上的**路径**被抹掉（`lea` 派生退回"只剩最后一跳"）⇒ 二级对象整类掉出所有桶（守恒③ 当场差数）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '      if (path.length + 1 > 2) { pointeeMulti += 1; multiHopSites.push(s); continue; }',
    to: '      if (false) { pointeeMulti += 1; multiHopSites.push(s); continue; }',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ "跨两级以上**不判**"被拆掉 ⇒ 三跳链被拿 `path[0]`（第一跳）去硬判 = **认错层**（早先那个假解释的来源）',
  },
  {
    file: 'tools/lib/shape-propagate.mjs',
    from: "    if (c.kind === 'arg' && c.fn && c.reg === 'ecx') {",
    to: '    if (c.fn && c.reg) {',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ local 载体被当成"函数的根类型"种子 ⇒ 类型从"某个寄存器"污染成"这个类是 X"，假冲突与错声明一起出现（★⑮ 当场红）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: "      if (d.kind === 'ptr-unknown' && m.kind === 'ptr' && m.type)",
    to: '      if (false)',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 争议台账不再检查"过期"（声明已经回答了争议却留着旧账）⇒ 争议越积越多、没人知道哪条已被解决（★⑯ 当场红）',
  },
  {
    file: 'tools/lib/shape-calls.mjs',
    from: "        if (ct && calleeType) verdict = ct === calleeType ? 'same' : (calleeIsAdapter ? 'adapter' : 'mismatch');",
    to: '        if (ct && calleeType) verdict = \'same\';',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 调用链对账不再区分"不一致"（两侧类型不同也判 same）⇒ 跨函数的类型冲突永远发现不了（★⑭ 当场红）',
  },
  {
    file: 'tools/lib/shape-coverage.mjs',
    from: "  if (escapes.indirectJmp.length) reasonKinds.push({ kind: 'indirectJmp', count: escapes.indirectJmp.length, text: `${escapes.indirectJmp.length} 处**间接跳转**（目标未知）` });",
    to: '  if (false) reasonKinds.push({ kind: \'indirectJmp\', count: escapes.indirectJmp.length, text: \'x\' });',
    guard: 'tools/test/shape-coverage.assets.test.mjs',
    what: '★ "目标未知的间接跳转"不再算未闭合原因 ⇒ 跳表函数被判成 **L0 闭合**（"这个函数分析完了吗"当场变假，归档会把它列进"下一层能用"）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '    if (off != null && count != null && /^rep\\s+stosd$/.test(t)) {',
    to: '    if (false) {',
    guard: 'tools/test/shape-claims-g4-init.test.mjs',
    what: '★ "初始化写循环"这条证据被拆掉 ⇒ `T_Command+0x77bec`（构造函数 `mov ecx,200h`+`rep stosd`）只能退回"只声明首槽"，索引访问随即变成假红；而 G4 的"条数有直接观察"这一档也就名存实亡',
  },
  {
    file: 'tools/lib/shape-coverage.mjs',
    from: '    if (PAD_LEA.test(r.body)) { kinds.pureReg += 1; continue; }',
    to: '    if (false) { kinds.pureReg += 1; continue; }',
    guard: 'tools/test/shape-coverage.test.mjs',
    what: '★ MSVC 的 6 字节对齐填充（`lea r,[r+0]`）又被算成"有内存操作数却没站点" ⇒ 一批函数永远闭不上（实测全表 147 个"未归类行"全是它 = 假红）',
  },
  {
    file: 'tools/lib/shape-coverage.mjs',
    from: '  if (REG_NAME.has(s.toLowerCase())) return false;',
    to: '  return true;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 闭合度不再区分"间接调用"（`call eax` 被当成直接调用）⇒ 副作用无界的函数会被判成 **L0 闭合**（"分析完成了吗"这个判据变假）',
  },
  {
    file: 'tools/lib/shape-access.mjs',
    from: "    if (mn === 'call' && NORETURN.test((operands[0] ?? '').trim())) { insns[k].succ = []; continue; }",
    to: '    if (false) { insns[k].succ = []; continue; }',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ `noreturn` 之后的 fall-through 又被连上 ⇒ 抛出路径上的写污染汇合点，汇合后面的访问整类判成"未解析基址"（实测：sub_41C6A0 的 9 处）',
  },
  {
    file: 'tools/lib/shape-access.mjs',
    from: '        if (!dm.explicit || dm.explicit === 4) fr.set(slotKey(dm), evalOperand(fr, s));',
    to: '        if (false) fr.set(slotKey(dm), evalOperand(fr, s));',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 栈槽的值传播被拆掉 ⇒ 对象根溢出/读回后身份断掉，catch 块里的访问全成"未解析基址"（实测：sub_429460 的 8 处）',
  },
  {
    file: 'tools/lib/shape-census.mjs',
    from: '    if (scalar.has(o.abs) || finite.some(([a, b]) => o.abs >= a && o.abs < b)) out.declared.push(o);',
    to: '    if (scalar.has(o.abs) || finite.some(([a, b]) => o.abs >= a && o.abs <= b)) out.declared.push(o);',
    guard: 'tools/test/shape-census-debt.test.mjs',
    what: '★ 欠账窗口改成**闭区间** ⇒ 窗口末尾的下一格被当成"已声明"（真欠账被吞掉一格，"还要补什么"的清单少报）',
  },
  {
    file: 'tools/lib/shape-access.mjs',
    from: '          unusable: sh.problems.length > 0,',
    to: '          unusable: false,',
    guard: 'tools/test/shape-access.test.mjs',
    what: '★ 索引解不出的 `lea` 目标寄存器又被当成 `this+0` ⇒ 下游 `[reg+K]` 变成 `obj0+K` 的**假偏移**（实测 0x422527 ⇒ 0x4225b5 起：5 个偏移 / 32 处站点被当成"未声明成员"，而基址其实是 `Engine + (跳表值−序号)*4`）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '    if (hit.brokenByCall) {',
    to: '    if (false) {',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 局部载体不再拒绝"活窗口里被 call 打断的 caller-saved 寄存器" ⇒ 值可能已被抹掉的断言照样被接受（**假绿**）',
  },
  // ── shape 域（第五组）：**全局载体（G13）与"从根正向传播"** —— 根对象不是参数，只能从数据段进来 ──
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '        if (storeRe.test(text)) stores.push({ ea, text });',
    to: '        if (true) stores.push({ ea, text });',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G13 不再要求 witness 是**写**该全局的 store ⇒"这个全局装的是 X 指针"退化成"随便给个 EA 就算数"（根对象那种断言的唯一机械证据没了）',
  },
  {
    file: 'tools/lib/shape-propagate.mjs',
    from: "      else if (arg.k === 'global') t = GLOB.get(arg.sym) ?? null;",
    to: "      else if (arg.k === 'global') t = null;",
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 传播里"全局槽里装的是什么类"这条规则被拆掉 ⇒ 从根正向的传播整类失效（`_WinMain`/WndProc 那一整片入口永远进不了图，只能继续从 op handler 倒着爬）',
  },
  {
    file: 'tools/lib/shape-access.mjs',
    from: "  if (segm && !segm[2].includes('[')) return { t: 'glob'",
    to: '  if (false) return { t: \'glob\'',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ `mov ecx, ds:<全局>` 不再被认成"全局里装着的东西" ⇒ 调用点上"ecx 来自根对象"整类退回 `unknown`（数字变小，不是报错）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '    if (t.adapter !== true) continue;',
    to: '    if (true) continue;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 适配器类型不再受三条硬要求约束（不许 size / 不许 complete / 至少一个成员）⇒ "部分布局"被说成完整对象，且"同偏移不同类 = 相容"这条事实失去落点',
  },
  {
    file: 'tools/lib/shape-calls.mjs',
    from: "        const calleeIsAdapter = calleeType ? types?.[calleeType]?.adapter === true : false;",
    to: '        const calleeIsAdapter = false;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 调用点对账不再认"适配器相容" ⇒ 适配器被四个类调用就报 4 处**假不一致**（或反过来，有人用它糊掉真不一致）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '      if (host.kind !== \'ptr\' && host.type && host.count === 1) {',
    to: '      if (false) {',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ "内嵌子对象的第一双字是 vtable"这条判据被拆掉 ⇒ 内嵌子对象的虚调用被当成"标量被解引用"⇒ 逼人把就地构造的子对象改声明成 `kind:"ptr"`（假声明换假绿）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '        if (elemOff === 0) { leaf = host; }',
    to: '        if (false) { leaf = host; }',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ "元素 0 的偏移 0 = 数组/内嵌对象自己的起点"被拆掉 ⇒ 每一条 `lea ecx,[esi+K]`（子对象基址）都变成"元素内偏移 0x0 不是成员"的残差（纯噪声淹掉真残差）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: "      if (s.problems?.includes('index') || (s.indexSym && !/^[A-Za-z_]\\w*$/.test(String(s.indexSym)))) { pointeeAmbiguous += 1; ambiguousEAs.push(s.ea); continue; }",
    to: '      if (false) { pointeeAmbiguous += 1; ambiguousEAs.push(s.ea); continue; }',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ "基址/索引分不清"这一档被拆掉 ⇒ `[ecx+eax]`（两个寄存器都是 load 出来的值）被当成"基址成员是指针"⇒ 逼人把整数游标声明成 `kind:"ptr"`（假绿）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '    const classifyIndexSite = (s) => {',
    to: '    const classifyIndexSite = (s) => { return null;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G15 整条失效 ⇒ 索引访问落在非数组成员上不再被点名（“1024 项分派表被写成 544 条离散槽”这类矛盾重新变回沉默）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '      const countWide = (m.count != null && Number.isFinite(num(m.count))) ? num(m.count) * elemSize : elemSize;',
    to: '      const countWide = elemSize;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 数组窗口只覆盖第 1 个元素（count 被忽略）⇒ 落在第 k>0 个元素的访问全部变残差（实测：分派表数组化后构造器那 544 条逐槽写从解释变残差）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '          ? (elemOffRaw % et.size)',
    to: '          ? elemOffRaw',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 元素内偏移不取模 ⇒ "第 k 个元素的第 0 个字段"被算成"元素 0 的偏移 k*stride"，只有 k=0 能对上（第 k>0 个元素全进残差）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '        if (t.size != null && end === t.size) {',
    to: '        if (false) {',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G4 第三档被拆掉 ⇒ "数组正好填满对象"（末尾==size，如 T_FileDB+0x3c48 的 256 格表）又变回红，诚实的数组声明只能退化成单槽 + note',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '        if (t.size != null && end === t.size) {',
    to: '        if (t.size != null && end <= t.size) {',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 第三档的等式被放宽成"末尾 ≤ size" ⇒ 任何没贴边、又零引用的编造 count 都能借这一档过关（贴边是可证伪的等式，不是不等式）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '        if (!rttiNames.has(name)) { g14.structural += 1; continue; }',
    to: '        if (true) { continue; }',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G14 命名棘轮整条失效 ⇒ `T_<Name>` 这个 id 又变成没人守的空话（名字写错 / 换一次导出 / note 过期都不会响；实测就是这样漏掉了 T_RemoteDebug 的过期 EA）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: "      const m = /不是\\s+(\\w+)\\s+的成员/.exec(w) ?? /落在内嵌子对象\\s+(\\w+)\\+/.exec(w);",
    to: '      const m = null;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ `--by-type` 不再从残差行取宿主类型（一律算成载体类型）⇒ 第二跳的残差被记到发起访问的类型上（会把 `T_Input+0x490` 派给 `T_Font`，写进 `T_Font` 的成员表 = 假声明）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '          const hostIsArray = host.count == null || host.count > 1;',
    to: '          const hostIsArray = false;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G3 不再对"真数组"校验元素大小 ⇒ 步长与元素大小不符的数组声明又变成沉默（`T_Scene`/`T_CBunki` 那种 count 写错的数组不会被抓）',
  },
  {
    file: 'tools/lib/shape-access.mjs',
    from: '    if (dst && dst === mem.base) return null;',
    to: '    if (false) return null;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ `lea r,[r+0]`（6 字节对齐填充）又被当成一次访问 ⇒ `obj0+0x0` 的假残差回来了，逼人给类型写一条 `{off:"0x0"}` 的假成员',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '        if (!cls.some((x) => x.kind === \'call\')) {',
    to: '        if (false) {',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G16 不再要求"至少一条 witness 落在调用点上" ⇒ 只凭"某行提到过 ecx"就能给载体背书（等于回到"witness 免检"的原点）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '          if (calls.size) { red(where, `声明了 indirectOnly',
    to: '          if (false) { red(where, `声明了 indirectOnly',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ `indirectOnly` 不再可反证 ⇒ 任何"我不想给 witness"的载体都能声明"只经分派表调用"来免检（把可证伪声明变成逃生口）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '            if (m.kind === \'ptr\') continue;      // ⛔ 指针成员不是容器',
    to: '            if (false) continue;      // 变异：指针成员被当容器',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ 指针成员被当成容器 ⇒ 宿主偏移正好等于某个 `ptr` 成员时会被解进它的**目标类型**（实测 `T_Sys3Draw+0x1fb0 → T_Scene` 会凭空多 17 处残差）',
  },
  {
    file: 'tools/lib/shape-access.mjs',
    from: "      pointeeOff: path.length ? (typeof sh.off === 'number' && sh.off < 0 ? null : sh.off) : null,",
    to: '      pointeeOff: path.length ? sh.off : null,',
    guard: 'tools/test/shape-access.test.mjs',
    what: '★ 折叠层的负位移又被写进 pointeeOff ⇒ 判定器把它记成「某类型缺 0x-4 这个成员」的假缺口（实测 T_5d8bc+0x-4 / 0x40EA97）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '      const endAbs = absBase != null ? absBase + et.size * m.count : null;',
    to: '      const endAbs = null;',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G4 退回用**类型相对偏移**查引用 ⇒ 内嵌子对象的数组末尾一律假红（实测 T_inihdr+0x0 报「末尾 0x4 零引用」，而绝对 0xAAA1C 有 2 处引用）',
  },
  {
    file: 'tools/lib/shape-access.mjs',
    from: '        return m ? mem.indices[0].scale * m : mem.indices[0].scale;',
    to: '        return mem.indices[0].scale;',
    guard: 'tools/test/shape-access.test.mjs',
    what: '★ 强度削减的乘数链被丢掉 ⇒ memScale 回退成裸 scale（实测 0x40F3B1 会报 8 而非 0x78，与 size 比对时双向出错）',
  },
  {
    file: 'tools/lib/shape-claims.mjs',
    from: '      if (Number.isInteger(st) && st > 0 && !arrays.some((x) => x.es === st)) {',
    to: '      if (false) {',
    guard: 'tools/test/shape-claims-guards.test.mjs',
    what: '★ G17 不再要求 axis.stride 有已声明数组背书 ⇒ 任何“我说这个游标步长是 N”都能通过（标注退化成口号）',
  },
  {
    file: 'tools/lib/shape-access.mjs',
    from: "      for (const rg of ['esi', 'edi', 'ecx', 'eax']) fr.set(rg, UNK);",
    to: '      // 变异：串指令不再失效寄存器',
    guard: 'tools/test/shape-access.test.mjs',
    what: '★ 串指令（rep movs*/stos*）不再让 esi/edi 失效 ⇒ rep 之后 [esi+disp] 被当成对象相对位移 = 假解释（实测 795 处）',
  },
];

/**
 * 入口（派发器 `tools/cli.mjs` 与直接运行都走这里）。
 * @returns {number} 退出码：0 = 全部按预期变红；1 = 有守卫没抓住破坏；2 = 还原失败（已中止）
 */
export function main(argv = process.argv.slice(2)) {
  // ★ `--match <子串>`：只跑匹配的变异（匹配 `file` / `what` / `guard`）。
  //   ★ 为什么必须有：本工具**会临时改写工作树里的文件** ⇒ 只想验"我这一域新加的守卫会不会红"时，
  //     不该被迫把全部变异跑一遍（窗口越长，越可能撞上别的进程在编辑）。技能文档里写的
  //     `pnpm tools mutate check --match shape` 原先**只是个不存在的 flag**（被静默忽略 ⇒ 跑全套）。
  const mi = argv.indexOf('--match');
  const match = mi >= 0 ? (argv[mi + 1] ?? null) : null;
  const selected = match ? MUTATIONS.filter((m) => `${m.file} ${m.what} ${m.guard}`.includes(match)) : MUTATIONS;
  // ★ `--help` 必须**只打用法就走**：它原先落到"跑全套变异"那条路（实测踩过一次）——
  //   而本工具**会临时改写工作树里的文件**，在有别的进程（人或 agent）正在编辑时跑，
  //   还原那一步可能把别人的改动一起写回去。
  if (argv.includes('--help') || argv.includes('-h')) {
    console.log(
      '用法：node tools/mutate-check.mjs [--list] [--match <子串>]\n\n' +
        '  不带参数 = 逐条施加变异 ⇒ 跑对应守卫 ⇒ 要求退出码非 0（最后逐条还原并比对全文）。\n' +
        '  ⛔ **它会临时改写工作树里的文件** ⇒ 有别的进程正在编辑本仓时**不要**跑（还原会覆盖别人的改动）。\n' +
        '  --list 只列清单（不动任何文件）。\n' +
        '  --match <子串> 只跑 file / what / guard 里含该子串的变异（例：--match shape）。',
    );
    return 0;
  }
  if (argv.includes('--list')) {
    for (const m of selected) console.log(`${m.what}\n    ${m.file}  ⇒ 守卫 ${m.guard}`);
    return 0;
  }
  if (match && selected.length === 0) {
    console.error(`没有匹配「${match}」的变异（--list 看清单）`);
    return 1;
  }

  const results = [];
  for (const m of selected) {
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

