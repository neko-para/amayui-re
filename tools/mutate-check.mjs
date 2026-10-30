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
    '**工作树里被守卫覆盖的源文件**（`packages/age-format/src/asm/*.mts` · `apps/emulator/src/model/*.ts` · `apps/emulator-new/src/**`）—— ★ **临时改写后必还原**',
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
  // ── `0x8 call-frame` 的**三格写入**（★★ 换帧是本轮新成篇的规则，判据必须会红）──
  //   ① 父帧号不写 ⇒ 目标帧结束后回到"当初装载它的那一帧"而不是调用者。
  //      表现：单层看起来正常，只有"装载者 ≠ 切帧者"时才错位 —— 所以判据里把两处分开了。
  {
    file: 'apps/emulator/src/vm/machine.ts',
    from: '    target.caller = this.cur;',
    to: '    void 0;',
    guard: 'tools/test/emulator-frames.test.mjs',
    what: '`callFrameAt` 不写父帧号（换帧把"最后切进来的那一帧"丢了）⇒ 目标帧结束后回到错的帧，而单层用例看起来一切正常',
  },
  //   ② 条件释放改成无条件 ⇒ `0x8` 切出来的（不相邻）目标帧被误释放，再切回去会"没有装载脚本"。
  {
    file: 'apps/emulator/src/vm/machine.ts',
    from: '    if (parent + 1 === this.cur) this.slots[this.cur] = null;',
    to: '    this.slots[this.cur] = null;',
    guard: 'tools/test/emulator-frames.test.mjs',
    what: '`returnToCaller` 丢掉"父帧号 + 1 == 当前帧号"这个条件 ⇒ 换帧切出来的已装载帧被误释放（跨槽返回必须留住它）',
  },
  // ── `apps/emulator-new` 的帧模型：**40 槽 + cur**（★★ 这一批判据的意义就是"栈实现会露馅"）──
  //   ① 把"回到父帧号 + 向下跳过空槽"退化成 `cur -= 1` ⇒ 跨槽返回落到相邻槽（相邻用例仍然绿）。
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    let next = parent;\n    while (next >= 0 && !this.slots[next]) next -= 1;\n    if (next < 0) throw new Error(\'exit：从父帧号往下全是空槽（引擎在帧号降到 0 以下时抛）\');',
    to: '    const next = this.cur - 1;',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '`0x2` 退化成"弹栈顶"（`cur -= 1`）：相邻调用仍绿，跨槽返回（父号 0、当前号 3）落到 2 ⇒ 栈模型的逃逸当场暴露',
  },
  //   ② 条件释放改成无条件 ⇒ `0x8` 切出来的目标帧被误释放。
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    if (parent + 1 === this.cur) this.slots[this.cur] = null;',
    to: '    this.slots[this.cur] = null;',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '`returnToCaller`（emulator-new）丢掉"父帧号 + 1 == 当前帧号"条件 ⇒ 换帧切出来的已装载帧被误释放',
  },
  //   ③ 换帧不写父帧号 ⇒ 目标帧结束后回到"当初装载它的那一帧"（装载者 ≠ 切帧者时才错）。
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    target.parent = this.cur;',
    to: '    void 0;',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '`callFrameAt`（emulator-new）不写父帧号 ⇒ "装载者 ≠ 切帧者"时目标帧回到错的那一帧（单层看起来正常）',
  },
  //   ④ 换帧不拨回指令指针 ⇒ 目标帧从"上次停的地方"继续（不是从它的开头）。
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    target.ip = 0;',
    to: '    void 0;',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '`callFrameAt`（emulator-new）不把目标帧 ip 拨回 0 ⇒ 换帧从"上次停的地方"继续执行（不是它开头）',
  },
  //   ⑤ 装帧顺手改 cur ⇒ "往槽 op2 写记录、装完不动当前帧"当场不成立。
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    while (this.slots.length <= slot) this.slots.push(null);\n    this.slots[slot] = rec;',
    to: '    while (this.slots.length <= slot) this.slots.push(null);\n    this.slots[slot] = rec;\n    this.cur = slot;',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '`loadFrameAt`（emulator-new）顺手改 `cur` ⇒ 装载帧把当前帧也切走了（"cur 不动"那条判据必须红）',
  },
  //   ⑥ 深度按"已占用的槽数"判（而不是**帧号**）⇒ 预装到 39 号槽后再换脚本会被放过。
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    if (slot >= FRAME_SLOT_COUNT) {\n      throw new Error(`call-script 深度超限',
    to: '    if (this.slots.filter(Boolean).length >= FRAME_SLOT_COUNT) {\n      throw new Error(`call-script 深度超限',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '`enterScript` 把深度判据换成"已占用槽数 ≥ 40" ⇒ 帧号越界被放过（这正是"帧号是数"那条判据）',
  },
  // ── `0x192 set-string` 的源侧（★ type 2 内联字符串：偏移 / 取反任一处错都只写错字节，不抛）──
  {
    file: 'apps/emulator-new/src/core/values.ts',
    from: '  const off = 4 * raw;',
    to: '  const off = raw;',
    guard: 'tools/test/emulator-new-string.test.mjs',
    what: '内联字符串的偏移漏掉 `4×`（写成 `raw`）⇒ 从指令流中间开始解，解出一串乱码/读到末尾才抛',
  },
  {
    file: 'apps/emulator-new/src/core/values.ts',
    from: '    const c = body[p] ^ 0xff;',
    to: '    const c = body[p];',
    guard: 'tools/test/emulator-new-string.test.mjs',
    what: '内联字符串少了 `^0xFF` ⇒ 字体名一类文本整体变成乱码（落点照样写成功，只有内容是错的）',
  },
  // ── `0x192` 的落点 / 源可以是指针族（8 / 14）★ 退回"只认 5 / 11" ⇒ GAME START 的 tag-14 直接失败 ──
  {
    file: 'apps/emulator-new/src/core/values.ts',
    from: "  if (op.type === 8 || op.type === 14) {\n    const r = deref(m, op);\n    if (r.tag === 5) {\n      m.stringPoolAt(r.bank).set(r.index, value);",
    to: "  if (op.type === 88) {\n    const r = deref(m, op);\n    if (r.tag === 5) {\n      m.stringPoolAt(r.bank).set(r.index, value);",
    guard: 'tools/test/emulator-new-string.test.mjs',
    what: '文本落点退回"只认 5 / 11" ⇒ 指针族落点（type 8 / 14）抛异常；实测症状是 GAME START 的 0x192 直接 halt',
  },
  // ── TITLE 场景的**合成顺序**（★ 退回"先所有绘制项、再所有网格" ⇒ key 0 黑色背景盖住前景，整屏黑）──
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-order.ts',
    from: '    if (a.drawable.key !== b.drawable.key) return a.drawable.key < b.drawable.key ? -1 : 1;',
    to: '    if (a.drawable.key !== b.drawable.key) return a.drawable.kind === \'item\' ? -1 : 1;',
    guard: 'tools/test/emulator-new-scene-order.test.mjs',
    what: '两类对象退回"绘制项全部在前、网格全部在后" ⇒ TITLE 的 key 0 黑色背景盖住 key 10 的前景（整屏黑），而 LOGO 的影片覆盖层仍可能正常',
  },
  // ── 同键的类别序：网格先于绘制项（★ 反了就变成绘制项压在网格下面）──
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-order.ts',
    from: "    if (a.drawable.kind !== b.drawable.kind) return a.drawable.kind === 'mesh' ? -1 : 1;",
    to: "    if (a.drawable.kind !== b.drawable.kind) return a.drawable.kind === 'item' ? -1 : 1;",
    guard: 'tools/test/emulator-new-scene-order.test.mjs',
    what: '同键时的类别序写反（绘制项先于网格）⇒ 违反 CORE-SCENE-DRAW-ORDER 的平键序；TITLE/LOGO 没有同键对象，只有合成用例能抓住',
  },
  // ── `0x9` 的复位路径必须清 show-logo（★ 只清字段不够，重跑的根脚本 `0x130` 必须读到 0）──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.showLogo = 0;',
    to: '  m.showLogo = 1;',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '`0x9` 的复位路径不清 show-logo（或写成 1）⇒ 重跑的根脚本仍经 `0x130` 读到 1、再次播放 LOGO；现有 prologue 用例是手写 `m.showLogo=0`，抓不到',
  },
  // ── `--skip-logo` 的参数解析（★ 解析坏了 ⇒ 静默回到初值 1）──
  {
    file: 'apps/emulator-new/src/platform/node/cli.ts',
    from: "    else if (a === '--skip-logo') out.skipLogo = true;",
    to: "    else if (a === '--skip-logo') out.skipLogo = false;",
    guard: 'tools/test/emulator-new-cli.test.mjs',
    what: '`--skip-logo` 被解析成 false ⇒ CLI 仍按初值 1 走 LOGO（真实启动链用例直接传 `showLogo:0`，抓不到参数层）',
  },
  // ── `--skip-logo` 必须真的把 show-logo 初值传进 Machine（★ 丢了就仍走 LOGO，且只在真实启动链上显形）──
  {
    file: 'apps/emulator-new/src/core/runner.ts',
    from: '    scriptName: opts.scriptName, rootId: opts.rootId, rootName: opts.rootName, showLogo: opts.showLogo,',
    to: '    scriptName: opts.scriptName, rootId: opts.rootId, rootName: opts.rootName, showLogo: 1,',
    guard: 'tools/test/emulator-new-title.assets.test.mjs',
    what: '`showLogo` 被写死 1 ⇒ `--skip-logo` 失效、启动链仍进入 LOGO.BIN（单测 0x130 仍绿，只有真实启动链会红）',
  },
  // ── 机制归属（★ planned 的 mechanism 丢了，机制表 ↔ 需求树的守卫应当红）──
  {
    file: 'data/spec/audit.json',
    from: '      "id": "containers",\n      "title": "容器句柄：懒分配 / 登记 / 元素寻址",\n      "status": "partial",\n      "mechanism": "CORE-POOL",',
    to: '      "id": "containers",\n      "title": "容器句柄：懒分配 / 登记 / 元素寻址",\n      "status": "partial",',
    guard: 'tools/test/spec-mechanisms.test.mjs',
    what: 'planned 的 `objects` 丢了 `mechanism` ⇒ 机制 ↔ planned ↔ 需求树的对齐不再可判（守卫必须点名）',
  },
  // ── 操作数类型表（★ 取址原语的 case 7 若没有标签，解码器遇到它就是抛/错位）──
  {
    file: 'packages/age-format/src/asm/types.mts',
    from: '    case 7: return \'global-float-ptr\';',
    to: '    case 7: throw new Error(\'mutated: no label\');',
    guard: 'tools/test/engine-operand-types.test.mjs',
    what: '取址原语的 case 7 没有标签 ⇒ `getTypeLabel(7)` 抛，而引擎的 12 个 case 里有 7（解码器会把合法操作数解成错误）',
  },
  // ── `0x192` 的注册（★ 没有 handler 会变成"未实现"停点 ⇒ 启动链停在一个误以为的缺口上）──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  [0x1a9, saveString1a9],\n  [0x192, setString192],',
    to: '  [0x1a9, saveString1a9],',    guard: 'tools/test/emulator-new-string.test.mjs',
    what: '`0x192` 没注册 ⇒ 内联串写不进串池（启动链把它当成"未实现"停点，而缺口报告看起来合法）',
  },
  // ── 数组连写 / 跳转 / 比较（★ `INITCONFIG4` 那一簇：错一处都让后面的循环次数/落点静默错）
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  const bits = encodeInt(0, m.key) >>> 0;',
    to: '  const bits = 0;',
    guard: 'tools/test/emulator-new-array-fill.test.mjs',
    what: '`0x6c fill-zero` 真填字面 0 ⇒ 整型池里存的不是 `ENC(key,0)`，读回来是 0 但位模式已错（后续按位拷贝的路径全偏）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (count > 0) for (let i = 0; i < count; i += 1) writeBitsAt(m, addr, i, bits);\n  m.effects.push({\n    kind: \'array.fill\',\n    detail: { opcode: \'0x6c\'',
    to: '  if (count > 0) for (let i = 0; i <= count; i += 1) writeBitsAt(m, addr, i, bits);\n  m.effects.push({\n    kind: \'array.fill\',\n    detail: { opcode: \'0x6c\'',
    guard: 'tools/test/emulator-new-array-fill.test.mjs',
    what: '`0x6c` 的循环写成 `<=` ⇒ 多写一格（把相邻格覆盖掉，而"填了 N 格"看起来没问题）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  const bits = encodeInt(value, m.key) >>> 0;',
    to: '  const bits = value >>> 0;',
    guard: 'tools/test/emulator-new-array-fill.test.mjs',
    what: '`0x2d8 set-array-to` 填 op2 原值（不编码）⇒ 读回来是 `DEC(原值)`，通常是一个巨大的错数',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (target === -1) return; // 哨兵 = "没有目标" ⇒ 本条原地落下（不写长度槽）',
    to: '  if (false) return;',
    guard: 'tools/test/emulator-new-array-fill.test.mjs',
    what: '`0x8c jmp` 不认 −1 哨兵 ⇒ `ip = 4×(−1) = −4`（下一次解码落在脚本外）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  writeInt(m, ops, 1, a < b ? 1 : 0);',
    to: '  writeInt(m, ops, 1, b < a ? 1 : 0);',
    guard: 'tools/test/emulator-new-array-fill.test.mjs',
    what: '`0x5c lt` 的比较方向写反 ⇒ 循环条件永远相反（跑完/跑飞，都不报错）',
  },
  // ── 指针 / 地址空间（★ `0x61` 的判据：步长、合成基址、解引用、配置键换算）
  {
    file: 'apps/emulator-new/src/core/address.ts',
    from: '    case 6: case 7: case 12: case 13: return 4;\n    case 8: case 14: return 28;',
    to: '    case 6: case 7: case 12: case 13: return 4;\n    case 8: case 14: return 4;',
    guard: 'tools/test/emulator-new-pointer.test.mjs',
    what: '字符串引用/指针的数组步长写成 4（应为 28）⇒ 每条字符串指针都指到另一条记录中间',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  const base = addressOfOperand(m, ops, 2);\n  const index = readInt(m, ops, 3) | 0;\n  const addr = (base + stride * index) >>> 0;',
    to: '  const base = addressOfOperand(m, ops, 2);\n  const index = readInt(m, ops, 3) | 0;\n  const addr = (base + index) >>> 0;',
    guard: 'tools/test/emulator-new-pointer.test.mjs',
    what: '`0x61` 的地址算术漏掉元素宽度（`base + index`）⇒ 下标被当成字节数，写到第 N/4 格',
  },
  {
    file: 'apps/emulator-new/src/core/values.ts',
    from: '  const addr = pointerSlotAddress(m, op.type as PoolTag, op.raw);\n  return resolveAddress(addr);',
    to: '  const addr = op.raw;\n  return resolveAddress(addr);',
    guard: 'tools/test/emulator-new-pointer.test.mjs',
    what: '指针解引用把"槽里的地址"当成"槽下标"（`op.raw`）⇒ 读到地址 0/1/2 那一带（未映射或别的池）',
  },
  {
    // ★ 唯一化：`configKey` 里"值族直接拼下标"那条也是同一个 `return` 形状，所以改的是**指针族**那条的
    //   最终下标来源（`r.index` = 地址换算的结果）；把它换成操作数自带的槽下标 ⇒ 键落到另一格。
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  return String.fromCharCode(code) + r.index.toString(16).padStart(8, \'0\');',
    to: '  return String.fromCharCode(code) + op.raw.toString(16).padStart(8, \'0\'); // 变异：键下标用指针槽下标',
    guard: 'tools/test/emulator-new-pointer.test.mjs',
    what: '配置键对指针族按"槽下标"算（而不是地址换算）⇒ `save-int` 写进另一个键，读回时静默得到 0',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  const off = 4 * raw;',
    to: '  const off = raw;',
    guard: 'tools/test/emulator-new-pointer.test.mjs',
    what: '`0x64` 的块首偏移漏掉 `4×`（`raw` 当成字节）⇒ 从指令流中间读"元素个数"，拷出一堆垃圾',
  },
  // ── 启动链前段（通往 LOGO.BIN 入口的那一簇）
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '    if (cand.length === key.length && cand === key) { hit = i; break; }',
    to: '    if (cand.startsWith(key)) { hit = i; break; }',
    guard: 'tools/test/emulator-new-prologue.test.mjs',
    what: '`0x2de` 的查表把"长度相等 + 逐字节相等"改成前缀匹配 ⇒ 配置的短名会命中（"MS" 命中 "MS Gothic"），回退分支不再触发',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  const key = raw.startsWith('@') ? raw.slice(1) : raw;",
    to: '  const key = raw;',
    guard: 'tools/test/emulator-new-prologue.test.mjs',
    what: '`0x2de` 不跳过键首的 `@` ⇒ 带 `@` 的字体名全部"未命中"（走回退分支，静默换字体）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  return (((x & 0xff) << 16) | (x & 0xff00) | ((x >>> 16) & 0xff)) >>> 0;',
    to: '  return x >>> 0;',
    guard: 'tools/test/emulator-new-prologue.test.mjs',
    what: '`0x76/0x77` 写成原值（不倒 24 位内的字节序）⇒ 颜色/索引值静默错通道',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (value > 31) throw new Error(`opcode 0xfe 的操作数',
    to: '  if (false) throw new Error(`opcode 0xfe 的操作数',
    guard: 'tools/test/emulator-new-prologue.test.mjs',
    what: '`0xfe` 去掉引擎自己的范围检查（>0x1f）⇒ 越界值被静默写进标量，而不是按引擎抛',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  scalarArray(m, '0x107', 'Engine.d551', readInt(m, ops, 1), readInt(m, ops, 2), 0x1f, 'skip');",
    to: "  scalarArray(m, '0x107', 'Engine.d551', readInt(m, ops, 2), readInt(m, ops, 1), 0x1f, 'skip');",
    guard: 'tools/test/emulator-new-prologue.test.mjs',
    what: '`0x107` 把索引与值两个操作数对调 ⇒ 写进另一格、值也错（表看起来仍被写）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.ip += 4; // 引擎自己加的那 4 字节（队列空 ⇒ 尾调不改 cur/ip）',
    to: '  m.ip += 0; // 错误：不推进',
    guard: 'tools/test/emulator-new-prologue.test.mjs',
    what: '`0x143` 不推进 ip（引擎在体内 `add [ip],4`）⇒ 下一条又执行自己（死循环），而长度槽已是 0',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    if (target.id !== undefined) this.enteredIds.push(target.id);',
    to: '    void 0;',
    guard: 'tools/test/emulator-new-prologue.test.mjs',
    what: '换帧进入的脚本不记账 ⇒ "到达 LOGO.BIN"这条断言失去依据（启动链看起来正常）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  if ((m.scalars.get('Engine.d95780') ?? 0) !== 0) {",
    to: '  if (false) {',
    guard: 'tools/test/emulator-new-prologue.test.mjs',
    what: '`0xae` 不再区分"存档版本修复"分支 ⇒ 未建模的那一支被当成早退（静默跳过修复）',
  },
  // ── 两个输入通道的大小写口径（★ 默认表折叠 / 玩家文件字节精确）
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '      const row = this.defaultByKey.get(p.key.toUpperCase());',
    to: '      const row = this.defaultByKey.get(p.key);',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '默认表键名不折叠大小写 ⇒ 索引里的小写键名静默落空（默认值不生效，而"读到一个 0"看起来正常）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '      const got = map.get(`${row.section}\\\\${row.key}`);',
    to: '      const got = map.get(`${row.section}\\\\${row.key}`.toLowerCase());',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '玩家文件匹配改成折叠大小写 ⇒ 规范要求的"字节精确"被放宽（错大小写也被入库，而看起来只是"读到了值"）',
  },
  // ── 玩家文件的字段切分 / 裁剪（★ 逐字节取证：顶层 `=` / 剥引号门槛 / 只剥一层 / 末行丢弃 / 引号屏蔽）──
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '  while (a < b && s.charCodeAt(a) <= 0x20) a += 1;',
    to: '  while (a < b && s.charCodeAt(a) <= 0x1f) a += 1; // 变异：0x20 不再算裁剪边界',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '首端裁剪漏掉 0x20（空格）⇒ `  ScreenMode = …` 的键带着前导空格，永远匹配不到词表（默认值静默留下）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '  if (a > 0 && b > 0 && s.charCodeAt(a) === 0x22 && s.charCodeAt(b - 1) === 0x22) {',
    to: '  if (b > 0 && s.charCodeAt(a) === 0x22 && s.charCodeAt(b - 1) === 0x22) { // 变异：去掉起始下标门槛',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '剥引号不再要求开引号前有被裁掉的字节 ⇒ `Font="A"` 的值从 `"A"` 变成 `A`（引号被静默吃掉）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '    return s.slice(a + 1, Math.max(a + 1, b - 1));',
    to: '    return trimField(s.slice(a + 1, b - 1)); // 变异：剥完再递归裁剪',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ 剥完一层又递归裁剪 ⇒ 引号内的空格被吃掉，由「引号内空格保留」（`"  A  "` ⇒ `A`）当场抓住',
  },
  // ── 段头的括号搜索要跳过落在双字节字符内部的命中（★ CP932 后继字节区间 0x40–0x7E 与 `]` 重叠）──
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '    if (line.charCodeAt(pos) === 0x5d && !insideDoubleByte(line, pos)) return pos;',
    to: '    if (line.charCodeAt(pos) === 0x5d) return pos; // 变异：不判双字节边界',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ 段名里双字节字符的第二个字节恰为 0x5D 时被当成结束括号 ⇒ 字符被截掉半个、段名从此匹配不到词表（覆盖静默失效）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from:
      'function insideDoubleByte(s: string, pos: number): boolean {\n' +
      '  const cur = s.charCodeAt(pos);\n' +
      '  if (isCp932LeadByte(cur) && pos + 1 < s.length && isCp932TrailByte(s.charCodeAt(pos + 1))) return true;\n' +
      '  return pos > 0 && isCp932LeadByte(s.charCodeAt(pos - 1)) && isCp932TrailByte(cur);\n' +
      '}',
    to:
      'function insideDoubleByte(s: string, pos: number): boolean {\n' +
      '  let inside = false; // 变异：改成从行首配对\n' +
      '  for (let i = 0; i < pos; ) {\n' +
      '    if (isCp932LeadByte(s.charCodeAt(i))) { inside = i + 1 === pos; i += 2; } else { inside = false; i += 1; }\n' +
      '  }\n' +
      '  return inside;\n' +
      '}',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ 把「局部判定」换成「从行首配对」⇒ 连续两个前导字节后再跟 `0x5D` 时后者被当成独立的 `]`（段名被提前截断，而单对输入看不出差别）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '  for (let pos = line.length - 1; pos >= 0; pos -= 1) {',
    to: '  for (let pos = 0; pos < line.length; pos += 1) { // 变异：改找第一个 `]`',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ 段头取第一个 `]` 而不是最后一个 ⇒ `[a]b]` 的段名从 `a]b` 变成 `a`（后续整段覆盖静默落空）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '    if (!lf && chunk.length < LINE_MAX) break; // EOF 前的残行：整行丢弃',
    to: '    if (false) break; // 变异：EOF 前的残行也收下',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '末行没有 LF 也收 ⇒ 与引擎的读行器相反（残行被当成一行解析，而文件看起来"只是少个换行"）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '    if (paren === 0 && quote % 2 === 0 && c === 0x3d) {',
    to: '    if (paren === 0 && c === 0x3d) { // 变异：引号不再屏蔽分隔符',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '引号不再屏蔽 `=` ⇒ `"Font=a` 被切成键 `"Font`、值 `a`（引擎把整行当一个键）',
  },
  // ── 配置键的 UB 判据（★ 规范明确：引用不在该族全局池 ⇒ 键没有定义）
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (r.tag !== wantTag) {',
    to: '  if (false) {',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '配置键不再检查"引用落在该族全局池"⇒ 局部指针（或别的池）被算成一个取决于布局的键，而规范把它定为 UB',
  },
  // ── 越界口径：`0x107` 是"静默跳过"、`0x30a` 是"抛"    （★ 统一它们会把"引擎会崩"与"无事发生"变成同一种表现）──
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
  // ── 需求台账：正文里的 `repo:` 路径引用必须真的存在（★ 塌了 ⇒ 路径前缀只剩"写着好看"）──
  {
    file: 'tools/lib/requirements.mjs',
    from: 'const PATH_REF_RE = /\\b(repo|old):([^\\s`]+)/g;',
    to: 'const PATH_REF_RE = /\\b(old):([^\\s`]+)/g;',
    guard: 'tools/test/requirements.test.mjs',
    what: '`repo:` 不再被识别成路径引用 ⇒ 指向不存在的本仓路径照样通过（前缀退化成"写着好看"，而树上没有任何提示）',
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
  {
    file: 'data/spec/chapters/10-format.md',
    from: '### 核验状态\n',
    to: '### 核验状态（全部已核验）\n',
    guard: 'tools/test/spec-format-table.test.mjs',
    what: '★ 协议章声称「全部已核验」 ⇒ 读者会把未核验的格式当已证实结论',
  },
  {
    file: 'tools/lib/spec-effects.mjs',
    from: "    if (!hit) { hit = { how: 'unregistered' }; unregistered.push(e); }",
    to: "    if (!hit) hit = { how: 'covered' }; // 变异：未登记 effect 冒充覆盖",
    guard: 'tools/test/spec.test.mjs',
    what: '★ effect 级闭合不再判"未登记" ⇒ 去掉 covers 的 effect 照样绿（审计退化成"有一份 spec 就算完"）',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: "    if (e.status === 'reviewed' && weakHere > 0)",
    to: '    if (false)',
    guard: 'tools/test/spec.test.mjs',
    what: '★ reviewed 允许夹 weak（proposed）证据 ⇒ 没核过的结论可以贴着 reviewed 标签进主交付物',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: '  if (!member) {',
    to: '  if (false) { // 变异：成员起点不再校验',
    guard: 'tools/test/spec.test.mjs',
    what: '★ `shape:<类型>@0x<偏移>` 不再校验"该偏移恰是成员起点" ⇒ 指针可以指向不存在的槽，而 #6 仍绿',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: '          else if (scopeTokens.size > 0 && !scopeTokens.has(tok)) {',
    to: '          else if (false) {',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 放电（replaced）不再判越界 ⇒ preserves/discards 可以指向 scope 之外的机器范围而全绿',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: '      if (!r.ok) { evp.push(`${at}：${r.why}`); continue; }',
    to: '      if (false) { continue; } // 变异：失效证据被跳过',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 失效证据被静默跳过 ⇒ 引一条 retracted / 不存在的台账记录也不会让条目变红（依赖失效不传播）',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: "    if (c.status === 'bootstrap' && n === 0)",
    to: '    if (false)',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 章节状态不再与条目对账 ⇒ bootstrap 可以是一张空壳、none 章里可以藏合同（"整个 spec 到哪一步"就变成自称）',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: "    if (e.status === 'reviewed' && !isNonEmptyArray(e.falsifiers))",
    to: '    if (false)',
    guard: 'tools/test/spec.test.mjs',
    what: '★ reviewed 不再要求可证伪预言 ⇒ 无法被观察推翻的合同也能进主交付物，且没人能说"什么会证明它错"',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: "          if (!hasSite) state.push(`${at} 的 covers 选择器指向块 ${b}，但 binding.sites 里没有它（机器见证缺失）`);",
    to: '          // 变异：不查见证',
    guard: 'tools/test/spec.test.mjs',
    what: '★ covers 选择器的块不再要求出现在 binding.sites ⇒ 覆盖与机器绑定脱钩',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: '        if (!boundTokens.has(item)) state.push(`${at} 的 replacement.scope 越界：${item} 不在 binding.sites / binding.functions 里`);',
    to: '        // 变异：scope 不查是否属于本条目',
    guard: 'tools/test/spec.test.mjs',
    what: '★ replacement.scope 可以凭空写（不在 binding 里的 EA）⇒ 放电范围成了自由文本',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: '        if (!declared.has(item)) state.push(`${at} 的 replacement.scope 有未交代项：${item}（必须进 preserves / discards / residual）`);',
    to: '        // 变异：scope 的未交代项静默消失',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 放电不再要求交代完 scope ⇒ 没被 preserves/discards 覆盖的机器范围静默消失',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: "      if (r.kind === 'omitted') {",
    to: '      if (false) {',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 残差 omitted 不再要求可解析的强证据 ⇒ 自由文本（或 proposed）就能把未知洗成省略',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: "      if (r.kind === 'discarded') {",
    to: '      if (false) {',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 残差 discarded 不再要求 reason + range ⇒ 舍弃可以说得含糊，且不落到机器范围',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: "    if (e.status === 'reviewed' && reviewable === 0) {",
    to: '    if (false) {',
    guard: 'tools/test/spec.test.mjs',
    what: '★ reviewed 可以只靠 assets/external 守卫（默认门禁不跑）⇒ "核过"里混进从未在门禁上跑过的证据',
  },
  {
    file: 'tools/lib/manifest.mjs',
    from: '          } else if (NOT_CARRYING.has(e.storage)) {',
    to: '          } else if (false) {',
    guard: 'tools/test/corpus-manifest.test.mjs',
    what: '★ external-only / deferred 的外部来源缺件不再只 warn ⇒ 没有旧仓/安装树的机器上 corpus validate 永远红（把"本机不可校验"当成"清单坏了"）',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: "  oneToOne('audit.json rules', audit?.rules, (a) => a?.id);",
    to: '  // 变异：audit 与 spec 不再一一对应',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 三份文件不再按 rule id 对账 ⇒ 少一条 audit/bindings 也能过（拆分后的孤儿/缺项静默）',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: '      if (m) prose.push(`chapter:${c.id} 含${why}：${m[0]}（分发产物必须自包含；复核信息放 bindings/audit）`);',
    to: '      // 变异：自包含检查不再报',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 规范正文混入汇编 / 台账 / 守卫引用也不再红 ⇒ 分发产物又把复核材料带出去',
  },
  {
    file: 'tools/lib/spec.mjs',
    from: '    if (!isNonEmptyArray(g.residual)) gapProblems.push(`${at} 缺 residual（缺口必须点名具体义务）`);',
    to: '    // 变异：缺口可以不点名义务',
    guard: 'tools/test/spec.test.mjs',
    what: '★ 审计缺口可以空着 residual ⇒ "未成篇"重新变成一句空话（没有具体义务、没有下一步）',
  },
  // ── 新章节的站点锚：窗口类注册的 EA 偏移一格 ⇒ 锚落到指令中间（规范正文仍"看起来有出处"）──
  {
    file: 'data/spec/bindings.json',
    from: '          "ea": "0x4baa02",',
    to: '          "ea": "0x4baa03",',
    guard: 'tools/test/spec-window-render-sites.assets.test.mjs',
    what: '★ 窗口类注册的站点 EA 偏移一格 ⇒ 机器绑定指到指令中间（正文自包含、校验器也不会替读者核这一条）',
  },
  // ── 未成篇 opcode 的落点：`hasHandler = 已写（规则标题）+ gaps.ops`（★ 漏列一个 ⇒ 义务重新"无人认领"）──
  {
    file: 'data/spec/audit.json',
    from: '        "0x4",\n        "0x7",',
    to: '        "0x7",',
    guard: 'tools/test/spec-gaps.test.mjs',
    what: '★ 未成篇 opcode 段漏列 `0x4`（有处理函数、但没有任何规则标题覆盖）⇒ 精确划分不成立；报告仍打印一个"自洽"的数字',
  },
  // ── 章首「本章 N 条规则」与 `<!-- rule:` 标记数同一份账（★ 只改声明、不改标记 ⇒ 章首计数立刻对不上）──
  {
    file: 'data/spec/chapters/65-save.md',
    from: '本章 9 条规则。',
    to: '本章 8 条规则。',
    guard: 'tools/test/spec-gaps.test.mjs',
    what: '★ 章首声明「本章 8 条规则」而文件里仍有 9 个 <!-- rule: --> 标记 ⇒ 章首计数与规则标记不是同一份账（spec-gaps 的章首规则计数必须红）',
  },
  // ── 0x000–0x0FF 段新增规则的站点锚：`0x8c` 的 handler 起点偏移一格 ⇒ 锚落到指令中间 ──
  {
    file: 'data/spec/bindings.json',
    from: '          "ea": "0x4203d0",',
    to: '          "ea": "0x4203d2",',
    guard: 'tools/test/spec-ops-000-sites.assets.test.mjs',
    what: '★ `0x8c` 的站点 EA 偏移一格 ⇒ 机器绑定指到指令中间（正文自包含、校验器也不会替读者核这一条）',
  },
  // ── 0x100–0x3FF 段新增规则的站点锚：换成一个不存在的 EA ⇒ 锚落空 ──
  {
    file: 'data/spec/bindings.json',
    from: '          "ea": "0x423b20",',
    to: '          "ea": "0x4f0001",',
    guard: 'tools/test/spec-ops-100-200-sites.assets.test.mjs',
    what: '★ `0x217` 的站点 EA 换成一个不存在的地址 ⇒ 机器绑定落空（正文自包含、校验器也不会替读者核这一条）',
  },
  {
    file: 'data/spec/bindings.json',
    from: '          "ea": "0x4baa11",',
    to: '          "ea": "0x4f0002",',
    guard: 'tools/test/spec-objects-sites.assets.test.mjs',
    what: '★ 根对象的分配站点 EA 换成一个不存在的地址 ⇒ CORE-OBJECTS 的机器绑定落空',
  },
  {
    file: 'data/spec/bindings.json',
    from: '          "ea": "0x437786",',
    to: '          "ea": "0x4f0003",',
    guard: 'tools/test/spec-save-sites.assets.test.mjs',
    what: '★ 存档头的魔数站点 EA 换成一个不存在的地址 ⇒ FMT-SAVE 的机器绑定落空',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: 'for (const off of [0x21c, 0x220, 0x224, 0x228, 0x22c, 0x230])',
    to: 'for (const off of [0x21c, 0x220, 0x224, 0x228, 0x22c])',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '★ `0x230` 少清一个共用格 ⇒ 附加组复位不完整，合同守卫必须当场红',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: 'm.frame.localMain = readInt(m, ops, 1) | 0;',
    to: 'm.frame.localMain = readInt(m, ops, 2) | 0;',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '★ `0x7b` 的主 / 副进入目标读反 ⇒ 两个位置静默互换，只有合同守卫能抓住',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: 'readFloat(m, ops, 2) - readFloat(m, ops, 3)',
    to: 'readFloat(m, ops, 2) + readFloat(m, ops, 3)',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '★ `0x2d1` 的减改成加 ⇒ 方向错误不会抛，只会静默算错；合同守卫必须当场红',
  },
  // ── 0x1a0 的七格映射：真实样本里"星期"（+268）与"日"（+270）不同值 ──
  //   ★ 把日读成星期不会抛：第 5 个操作数从 23 变成 3，只有拿真实头对值的守卫能抓住。
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '    readU16LE(bytes, 264), readU16LE(bytes, 266), readU16LE(bytes, 270),',
    to: '    readU16LE(bytes, 264), readU16LE(bytes, 266), readU16LE(bytes, 268),',
    guard: 'tools/test/emulator-new-save-read-fields.assets.test.mjs',
    what: '★ `0x1a0` 把星期（+268）当成日读进第 5 个操作数 ⇒ 日期整体错位而不抛，由真实样本头守卫抓住',
  },
  // ── 0x1ab / 0x1ac：两个文件都总是尝试（★ 第一个失败就返回 ⇒ 第二个文件不被碰、两个都失败也只报 1）──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from:
      "  if (!(await host.remove(host.fileForSlot(slot, 'dat')))) code = 1;\n" +
      "  if (!(await host.remove(host.fileForSlot(slot, 'sth')))) code = 2;",
    to:
      "  if (!(await host.remove(host.fileForSlot(slot, 'dat')))) code = 1;\n" +
      "  else if (!(await host.remove(host.fileForSlot(slot, 'sth')))) code = 2; // 变异：提前返回",
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '★ `0x1ab` 在 .DAT 失败处提前返回（不再删 .STH）⇒ 两个都失败也只报 1；守卫必须抓住「两个都尝试过 / 两个都失败 ⇒ 2」',
  },
  {
    file: 'data/spec/bindings.json',
    from: '          "ea": "0x41c1d1",',
    to: '          "ea": "0x4f0004",',
    guard: 'tools/test/spec-containers-sites.assets.test.mjs',
    what: '★ 空读分支的站点 EA 换成一个不存在的地址 ⇒ CORE-CONTAINERS 的机器绑定落空',
  },
  // ── emulator-new：整数减的**方向**（`第 2 个 − 第 3 个`）──
  //   ★ 写反不会报错、只会静默算错，所以必须有一条守卫专门钉这个方向。
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  writeInt(m, ops, 1, second - third);',
    to: '  writeInt(m, ops, 1, third - second); // 变异：减法方向写反',
    guard: 'tools/test/emulator-new-ops.test.mjs',
    what: '★ 0x51 的减法方向写反 ⇒ 10−3 变成 3−10（一切"看起来正常"，只有引擎数据错）',
  },
  // ── emulator-new：绘制项的矩形存"四条边"而不是宽高 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  item.right = right;',
    to: '  item.right = rightBase; // 变异：右边界退回"宽"',
    guard: 'tools/test/emulator-new-texture.test.mjs',
    what: '★ 0x1fb 把右边界存成"宽" ⇒ 矩形宽了 x 那么多（画得出来、位置错，且不报错）',
  },
  // ── emulator-new：AGF 的行宽 / 行序 / 调色板通道 ──
  {
    file: 'apps/emulator-new/src/core/agf.ts',
    from: '  return 4 * Math.ceil((bpp * width) / 32);',
    to: '  return (bpp * width) / 8; // 变异：去掉 32 位行对齐',
    guard: 'tools/test/emulator-new-agf.test.mjs',
    what: '★ AGF 行宽退回"不填充" ⇒ 8bpp 1026 宽按 1026 取行，所有像素错位',
  },
  {
    file: 'apps/emulator-new/src/core/agf.ts',
    from: '    const srcRow = (height - 1 - y) * stride;',
    to: '    const srcRow = y * stride; // 变异：不再翻转 bottom-up',
    guard: 'tools/test/emulator-new-agf.test.mjs',
    what: '★ AGF 不翻转 bottom-up ⇒ 图像上下颠倒（尺寸仍对、不报错）',
  },
  {
    file: 'apps/emulator-new/src/core/agf.ts',
    from: '        rgba[d] = palette[p + 2]!;',
    to: '        rgba[d] = palette[p]!; // 变异：调色板 R/B 互换',
    guard: 'tools/test/emulator-new-agf.test.mjs',
    what: '★ 8bpp 调色板 R/B 互换 ⇒ 红蓝通道错（像素数量对、颜色不对）',
  },
  // ── emulator-new：纹理尺寸必须来自解码图像，不是 0×0 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  m.scene.textureObjects.set(id, { kind: 'normal', width: image.width, height: image.height });",
    to: "  m.scene.textureObjects.set(id, { kind: 'normal', width: 0, height: 0 }); // 变异：尺寸写 0",
    guard: 'tools/test/emulator-new-texture.test.mjs',
    what: '★ 0x1f9 的纹理尺寸写 0 ⇒ 对象"存在"但取不到表面，绘制全落占位块',
  },
  // ── emulator-new：0x1f8 只写来源 / 空白位，颜色键不能清 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  m.scene.textures.set(id, { file: -1, color: previous?.color ?? 0, blank: 1 });",
    to: "  m.scene.textures.set(id, { file: -1, color: 0, blank: 1 }); // 变异：把颜色键清零",
    guard: 'tools/test/emulator-new-texture.test.mjs',
    what: '★ 0x1f8 把旧颜色键清零 ⇒ 新建纹理后颜色键静默丢失（现有测试都用新槽抓不到）',
  },
  // ── emulator-new：全屏降级（模拟器明确不支持全屏）──
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/presentation.ts',
    from: '  if (Number(params.exStyle) === 0) return params;',
    to: '  if (true) return params; // 变异：不再识别占满屏',
    guard: 'tools/test/emulator-new-presentation.test.mjs',
    what: '★ 占满屏请求不再降级 ⇒ 窗口仍是 1728×1117、画面按 1280×720 贴左上角（正是这次要修的症状）',
  },
  // ── emulator-new：影片循环位 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  obj.loop = (option & 1) !== 0;',
    to: '  obj.loop = false; // 变异：忽略循环位',
    guard: 'tools/test/emulator-new-movie.test.mjs',
    what: '★ 0x20f 忽略循环位 ⇒ 循环影片播完就被销毁',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '      if (obj.loop) {',
    to: '      if (false) { // 变异：循环分支不执行',
    guard: 'tools/test/emulator-new-movie.test.mjs',
    what: '★ 影片对象表服务不处理循环 ⇒ 循环影片被当成正常收尾',
  },
  // ── emulator-new：颜色窗把插值写回当前颜色（指数衰减）──
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '          item.colorNow = blendColor(item.colorFrom, item.colorTo, t);',
    to: '          item.colorFrom = blendColor(item.colorFrom, item.colorTo, t); // 变异：写回当前颜色',
    guard: 'tools/test/emulator-new-scene.test.mjs',
    what: '★ 绘制项把插值写回 colorFrom ⇒ 半程值变成指数衰减，且当前颜色被污染（正是版权页提前结束的原因）',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '        if (mesh.colorNow !== -1) mesh.colors = mesh.colors.map(() => mesh.colorNow >>> 0);\n        this.scene.animating = 1;',
    to: '        this.scene.animating = 1; // 变异：不重烘网格顶点色',
    guard: 'tools/test/emulator-new-scene.test.mjs',
    what: '★ 窗推进不重烘网格顶点色 ⇒ 网格仍画 0x322 的起色，渐变不可见',
  },
  // ── emulator-new：0x202 的"已存在"门 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if ((item.flags & 1) !== 0) {',
    to: '  if (true) { // 变异：去掉"已存在"门',
    guard: 'tools/test/emulator-new-scene.test.mjs',
    what: '★ 0x202 去掉"已存在"门 ⇒ 给不存在的绘制项也排窗（与 0x323 的差异被抹平）',
  },
  // ── emulator-new：取件顺序（松散优先）──
  {
    file: 'apps/emulator-new/src/core/install.ts',
    from: '    if (loose) {',
    to: '    if (false) { // 变异：丢掉"松散文件优先"',
    guard: 'tools/test/emulator-new-alf.test.mjs',
    what: '★ 丢掉松散优先 ⇒ 安装目录里被补丁覆盖过的脚本会被静默换成归档里的旧版（启动链量到另一份文件）',
  },
  // ── emulator-new：启动阶段的处置（配置只到 partial，不许被标成 modeled）──
  {
    file: 'apps/emulator-new/src/core/boot.ts',
    from: "    disposition: 'partial',\n    why: '配置库已建立",
    to: "    disposition: 'modeled', // 变异：把只到 partial 的配置标成已建模\n    why: '配置库已建立",
    guard: 'tools/test/emulator-new-boot-catalog.test.mjs',
    what: '★ 把"配置只到 partial"标成 modeled ⇒ 未建模的显示副作用被藏起来，脚本读配置时会被当成"配过了"',
  },
  // ── emulator-new：配置默认表的版本拆分 / 键分隔 / 行尾 / 布尔归一 ──
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: 'this.setInt(\'set:SaveVersion1\', Math.trunc(v / 100));',
    to: 'this.setInt(\'set:SaveVersion1\', Math.trunc(v / 10)); // 变异：版本除数写错',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ SAVEVERSION 的主版本除数 100 → 10 ⇒ 320 被拆成 32/20，版本号看起来仍"有值"',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: '    out.set(`${section}\\\\${trimField(topLevelField(raw, 0))}`, trimField(topLevelField(raw, 1)));',
    to: '    out.set(`${section}:${trimField(topLevelField(raw, 0))}`, trimField(topLevelField(raw, 1))); // 变异：段键分隔符写错',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ 玩家文件的段/键分隔符 `\\\\` → `:` ⇒ 所有覆盖都查不到，默认值原样留下（静默失效）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: "        parts.push(`[${section}]\\r\\n`);",
    to: "        parts.push(`[${section}]\\n`); // 变异：行尾只用 LF",
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ 保存的段头行尾 CRLF → LF ⇒ 写出的玩家文件是混合行尾（引擎按 LF 读，但真机工具会看作一行）',
  },
  {
    file: 'apps/emulator-new/src/core/config.ts',
    from: "      if (row.special === 'bool') v = v !== 0 ? 1 : 0;",
    to: "      if (row.special === 'bool') v = v; // 变异：布尔不归一",
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ FORCESCREEN 不归一（写原值 7 而不是 1）⇒ 消费方按 0/1 判断时把"开"读成未知',
  },
  // ── emulator-new：统一文件 id == TOC 条目下标 ──
  {
    file: 'apps/emulator-new/src/core/install.ts',
    from: '      return { index: this.index, entry: this.index.files[id]!, zone: 0 };',
    to: '      return { index: this.index, entry: this.index.files[id + 1]!, zone: 0 }; // 变异：id 偏一位',
    guard: 'tools/test/emulator-new-alf.test.mjs',
    what: '★ 统一文件 id 偏一位 ⇒ 根脚本取到 BGM 而不是 SYSTEM4.BIN（名字看起来仍是合法条目）',
  },
  // ── emulator-new：`0x8f call` 的操作数是**位置**（dword 序号）而不是字节偏移 ──
  {
    // ★ 唯一化：`m.ip = 4 * target;` 在 0x8f / 0xa0 / 0x8c 各有一处；带上前一行（压返回点）才只命中 0x8f。
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.frame.retStack.push(m.ip / 4 + 1 + 2 * ops.length);\n  m.ip = 4 * target;',
    to: '  m.frame.retStack.push(m.ip / 4 + 1 + 2 * ops.length);\n  m.ip = target; // 变异：把位置当字节偏移',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '★ 把"位置 × 4"写成"位置就是字节" ⇒ 调用跳到别处（脚本照跑、不报错，只是走错分支）',
  },
  // ── emulator-new：`0x2f8` 的音量夹取（饱和，不是回绕） ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  const clamped = volume < -10000 ? -10000 : volume > 10000 ? 10000 : volume;',
    to: '  const clamped = volume; // 变异：去掉夹取',
    guard: 'tools/test/emulator-new-voice.test.mjs',
    what: '★ 去掉音量夹取 ⇒ 大音量原样写进音量格（引擎是饱和到 ±10000）',
  },
  // ── emulator-new：`0x2f6` 的通道号 = 组号 + 固定偏移 ──
  {
    // ★ 唯一化：`0x2f8` 也有一行同样的通道号；带上下一条（后端门）才只命中 `0x2f6`。
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  const channel = group + VOICE_CHANNEL_BASE;\n  const backend = m.sound.backend === \'present\';',
    to: '  const channel = group; // 变异：丢掉通道偏移\n  const backend = m.sound.backend === \'present\';',
    guard: 'tools/test/emulator-new-voice.test.mjs',
    what: '★ 丢掉「组号 + 12」的通道偏移 ⇒ 释放请求与"在播"查询都落到另一条通道上（标志算反而不报错）',
  },
  // ── emulator-new：窗到期时绘制项是否清"有窗"位（★ 引擎与网格一样清；不清 ⇒ 场景永远"有动画"）──
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '        item.flags &= ~2;',
    to: '        // 变异：绘制项收尾不清"有窗"位',
    guard: 'tools/test/emulator-new-scene.test.mjs',
    what: '★ 绘制项到期不清位 ⇒ 场景永远"有动画"、等待门永远不打开（执行看起来只是"卡住"，不报错）',
  },
  // ── ALF / AGF：协议守卫本身必须真的"咬住"真实语料（改错常量 ⇒ 当场红）──
  {
    file: 'tools/test/spec-lzss.mjs',
    from: '    if (flags & mask) {',
    to: '    if (!(flags & mask)) { // 变异：flag 极性反了',
    guard: 'tools/test/spec-alf.assets.test.mjs',
    what: '★ flag 极性反过来 ⇒ 真实 *.AAI 解出的 TOC 全成 0（归档名、条目数全崩）——这正是本轮由守卫发现的 spec 歧义',
  },
  {
    file: 'tools/test/spec-alf.assets.test.mjs',
    from: 'const HEADER_BYTES = 268;',
    to: 'const HEADER_BYTES = 300; // 变异：把 268 当 300',
    guard: 'tools/test/spec-alf.assets.test.mjs',
    what: '★ 头长 300 ⇒ AC 的 12 字节块读错位置，真实索引解不出来；证明"268"是承重断言',
  },
  {
    file: 'tools/test/spec-agf.assets.test.mjs',
    from: 'const metaStart = 24;',
    to: 'const metaStart = 32; // 变异：头长改成 32',
    guard: 'tools/test/spec-agf.assets.test.mjs',
    what: '★ 头长 32 ⇒ meta 起错、body 头与 ACIF 全错位；证明"24 字节头"是承重断言',
  },
  // ── 规范指令表：`hasHandler` 与语料分派表的对应（★ 改一格 ⇒ 分发物判错"哪 30 条执行即抛"）──
  {
    file: 'data/spec/format/opcode-table.json',
    from: '{"opcode": 1, "argc": 0, "name": "abort", "aliases": [], "hasHandler": true}',
    to: '{"opcode": 1, "argc": 0, "name": "abort", "aliases": [], "hasHandler": false}',
    guard: 'tools/test/spec-isa.assets.test.mjs',
    what: '★ `hasHandler` 与语料分派表脱节 ⇒ 分发物把"有处理函数"的 opcode 判成"落缺省即抛"（表本身仍然格式合法）',
  },
  // ── emulator-new：窗口态风格字（★ 换成占满屏值 ⇒ 窗口态被当成全屏）──
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: 'export const WINDOWED_STYLE = 0x90cb0000;',
    to: 'export const WINDOWED_STYLE = 0x90000000; // 变异：窗口态用占满屏风格',
    guard: 'tools/test/emulator-new-window.test.mjs',
    what: '★ 窗口态风格字换成占满屏值 ⇒ 建窗参数把窗口态当全屏（外观/形态判据错而测试仍能跑）',
  },
  // ── emulator-new：周期定时器周期（★ 10000 → 1000 ⇒ 引擎常量被改小）──
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: 'export const PERIODIC_TIMER_PERIOD_MS = 10000;',
    to: 'export const PERIODIC_TIMER_PERIOD_MS = 1000; // 变异：周期缩小 10 倍',
    guard: 'tools/test/emulator-new-window.test.mjs',
    what: '★ 周期定时器 10000 → 1000 ⇒ 空闲抑制/激活补发频率变成 10 倍（引擎常量被当成仿真参数）',
  },
  // ── emulator-new：窗口宽 = 客户区 + 2×边框宽（★ 少乘 2 ⇒ 窗口比客户区还窄）──
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: '    const width = clientWidth + 2 * frame.frameWidth;',
    to: '    const width = clientWidth + frame.frameWidth; // 变异：边框只加一边',
    guard: 'tools/test/emulator-new-window.test.mjs',
    what: '★ 边框只加一边 ⇒ 窗口宽少 2×frameWidth（客户区被裁；引擎是“单边厚度 ×2”的对称假设）',
  },
  // ── emulator-new：0x20d 的 1000 阈值（★ 1000 → 1024 ⇒ 负数的无符号分支错）──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: 'const RENDER_TARGET_SLOT_LIMIT = 1000;',
    to: 'const RENDER_TARGET_SLOT_LIMIT = 1024; // 变异：阈值改成 1024',
    guard: 'tools/test/emulator-new-render.test.mjs',
    what: '★ 0x20d 的阈值 1000 → 1024 ⇒ 1000..1023 的纹理槽被当成后缓冲（负数的无符号分支也会变）',
  },
  // ── emulator-new：0x204 的“清开关”必须在入口条件之后（★ 提前清 ⇒ 条件不满足也改状态）──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  fontObject.switches[0] = 0;',
    to: '  fontObject.switches[0] = 1; // 变异：清成 1（不是 0）',
    guard: 'tools/test/emulator-new-render.test.mjs',
    what: '★ 请求第 1 步“清 0”被改成写 1 ⇒ 字体对象的共享开关状态错，而错误分支仍可能被当成成功',
  },
  // ── emulator-new：宿主时钟（★ 忽略 clock ⇒ 5 秒渐变被虚拟步长快进）──
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    this.now = this.clock ? this.clock() : this.now + ms;',
    to: '    this.now = this.now + ms; // 变异：忽略宿主时钟',
    guard: 'tools/test/emulator-new-scene.test.mjs',
    what: '★ 忽略 `MachineOptions.clock` ⇒ 窗口前端也按虚拟 tickMs 快进（渐变/等待不再按墙钟走）',
  },
  // ── emulator-new：CP932 显示/路径解码（★ 不解码 ⇒ 乱码目录 / 乱码字体名）──
  {
    file: 'apps/emulator-new/src/core/bytes.ts',
    from: '  return CP932.decode(encodeLatin1(text));',
    to: '  return text; // 变异：不按 CP932 解码',
    guard: 'tools/test/emulator-new-config.test.mjs',
    what: '★ 引擎字节串不按 CP932 解码 ⇒ macOS 上出现 `Eushully\\<乱码>` 目录，trace 里的字体名也仍是乱码',
  },
  // ── emulator-new：效果分级（★ 内部记账被当外部请求 ⇒ --trace 刷屏）──
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: "  if (INTERNAL_EFFECT_KINDS.has(kind)) return 'internal';",
    to: "  if (false) return 'internal'; // 变异：内部记账不再分级",
    guard: 'tools/test/emulator-new-render.test.mjs',
    what: '★ `string.set` / `array.lookup` 等内部记账不再归 internal ⇒ `--trace` 把已闭环的写入当外部副作用打印（刷屏且误导）',
  },

  // ── emulator-new：renderer 的 Host 路径（★ 不换分隔符 ⇒ macOS 上把 `\` 当地名字符）──
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/paths.ts',
    from: '  return decodeCp932(text).replace(/\\\\/g, sep);',
    to: '  return decodeCp932(text); // 变异：不把引擎反斜杠换成宿主分隔符',
    guard: 'tools/test/emulator-new-renderer-path.test.mjs',
    what: '★ renderer 的 `pathFromEngine` 不换分隔符 ⇒ POSIX 上 `Eushully\\天結…` 变成一个带反斜杠的单层目录（配置路径分叉）',
  },
  // ── emulator-new：运行期 key 生成（★ 缺省 0 ⇒ 引擎非法状态被当默认）──
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    this.key = opts.key ?? generateIntKey();',
    to: '    this.key = opts.key ?? 0;',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '未给 key 时默认 0 ⇒ 引擎不可能的状态被当默认（且 ENC(0)=0）',
  },

  // ── emulator-new：0x73 的 DrawMode 门（★ 不播种 ⇒ 默认 1 也误发第二请求）──
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: "    if (opts.config) this.scalars.set('Engine.d166964', opts.config.getInt('set:DrawMode'));",
    to: '    // 变异：不播种 DrawMode',
    guard: 'tools/test/emulator-new-subsys.test.mjs',
    what: 'renderer-init 不播种 set:DrawMode ⇒ 默认 1 仍按未建模/0 处理，误发 vtable.request',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (gate === 0) {',
    to: '  if (gate !== 0) {',
    guard: 'tools/test/emulator-new-subsys.test.mjs',
    what: '0x73 的 0x21 请求条件写反 ⇒ DrawMode=1 反而发请求',
  },

  // ── emulator-new：key=0 的入口诊断（★ 删掉它 ⇒ 非法引擎状态被静默接受）──
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: "    if (this.key === 0) this.diagnostics.push({ kind: 'codec-key-placeholder' });",
    to: '    // 变异：不记 key=0',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '宿主给 key=0 被静默接受 ⇒ 一次入口诊断那条判据当场红',
  },

  // ── emulator-new：UI 列表（0x70/0x73/0x80）与命名键 0x1ca ──
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: 'records: new Map(), defaultGroup: 1 };',
    to: 'records: new Map(), defaultGroup: 0 };',
    guard: 'tools/test/emulator-new-list.test.mjs',
    what: '默认组初值 1 → 0 ⇒ 组号 0 的 0x70/0x79 落到错组（初始状态下 0 与 1 应同组）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  return slot === 0 ? m.ui.defaultGroup : slot;',
    to: '  return slot;',
    guard: 'tools/test/emulator-new-list.test.mjs',
    what: '组号 0 不再替换成默认组 ⇒ 0x70/0x79 写到第 0 组而不是默认组',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  rec.f48 = (args[4] + args[6]) | 0;',
    to: '  rec.f48 = args[6];',
    guard: 'tools/test/emulator-new-list.test.mjs',
    what: '0x73 的 +0x48 漏掉 op5 ⇒ 十格公式错（12 变成 7）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  rec.f58 = 1;',
    to: '  rec.f58 = 0;',
    guard: 'tools/test/emulator-new-list.test.mjs',
    what: '0x73 的 +0x58 常量 1 写成 0 ⇒ 十格公式里唯一常量错',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  requireConfig(m).setInt('message:ReadTextSkip', readInt(m, ops, 1));",
    to: "  requireConfig(m).setInt('message:ReadTextSkipX', readInt(m, ops, 1));",
    guard: 'tools/test/emulator-new-forward.test.mjs',
    what: '0x1ca 写错命名键 ⇒ 读回/持久化都落到另一个键（看起来只是“没写进去”）',
  },

  // ── emulator-new：架构（★ core 搬回 main ⇒ renderer 只剩画布，逐指令 IPC 又会回来）──
  {
    file: 'apps/emulator-new/src/platform/electron/main.ts',
    from: 'import { ElectronWindowHost } ' + 'fr' + "om './window-host.ts';",
    to: 'import { Machine } ' + 'fr' + "om '../../core/machine.ts'; // 变异：main 重新 import core\nimport { ElectronWindowHost } " + "fr" + "om './window-host.ts';",
    guard: 'tools/test/emulator-new-isolation.test.mjs',
    what: '★ main 重新 import core ⇒ 架构守卫必须红（core 必须留在 renderer，main 只做窗口 / IPC / 生命周期）',
  },

  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  m.frame.retStack.push(m.ip / 4);",
    to: "  m.frame.retStack.push(m.ip / 4 + 1);",
    guard: 'tools/test/emulator-new-input.test.mjs',
    what: '0x100 的“当前位置重派发”写成下一条 ⇒ 多个按键只派发第一个、游标不再逐个前进',
  },

  // ── emulator-new：TITLE 的 `Version` 数字（0x23b CG 数字条）与颜色键 ──
  {
    file: 'apps/emulator-new/src/core/agf.ts',
    from: '    const argb = ((rgba[i + 3]! << 24) | (rgba[i]! << 16) | (rgba[i + 1]! << 8) | rgba[i + 2]!) >>> 0;',
    to: '    const argb = ((rgba[i]! << 16) | (rgba[i + 1]! << 8) | rgba[i + 2]!) >>> 0;',
    guard: 'tools/test/emulator-new-agf.test.mjs',
    what: '颜色键只比 RGB、丢掉 alpha ⇒ TITLE 的 Version 黑色描边被键 0 清成洞（"破裂"的根因）',
  },
  {
    file: 'apps/emulator-new/src/core/cg-digit.ts',
    from: '  const srcStride = rec.cellWidth + rec.innerGap;',
    to: '  const srcStride = rec.cellWidth + rec.advanceGap;',
    guard: 'tools/test/emulator-new-cg-digit.test.mjs',
    what: '0x23b 的源格子步长用错字段（字距 -3 代替字内空隙 2）⇒ 七位数字全取到相邻格子 / 越界，版本号错字',
  },
  {
    file: 'apps/emulator-new/src/core/cg-digit.ts',
    from: '      : zeroFill || k === last || remaining !== 0;',
    to: '      : zeroFill || k === last;',
    guard: 'tools/test/emulator-new-cg-digit.test.mjs',
    what: '0x23b 丢掉"剩余值 != 0"判据 ⇒ 右对齐时高位永远补 0（Version 会显示 0.00.0000 而看不出值）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  [0x23b, cgDraw23b],',
    to: "  [0x23b, unmodeledHandler(0x23b, 'adv-text')],",
    guard: 'tools/test/emulator-new-title.assets.test.mjs',
    what: '0x23b 退回未建模 ⇒ TITLE 的 Version 七位数字消失、只剩两个静态小数点（真实启动链守卫必须红）',
  },

  // ── emulator-new：0x2da / 0x23b / 四张同键表的读序与删除集合 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (id < 0 || id > 10) {',
    to: '  if (id < 0 || id > 999) {',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '0x2da 的编号上限写成 999（引擎是 `cmp edi,0Ah; ja`）⇒ 编号 11 = 12 被当成合法并去读其余七个操作数',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  return [m.scene.drawItems, m.scene.meshes, ...m.scene.auxTables];',
    to: '  return [m.scene.drawItems, m.scene.meshes, m.scene.auxTables[0]];',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '0x1f7 / 0x1f6 / 0x23b 共用的四张同键表少一张（漏掉第二张辅助表）⇒ 删除 / 清空集合不再一致',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  for (const slot of plan) {',
    to: '  if (plan.length === 0) { readInt(m, ops, 4); readInt(m, ops, 5); }\n  for (const slot of plan) {',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '0x23b 在位数 ≤ 0 时仍读 op4 / op5（引擎只在每个被画出的位的建项分支里读）⇒ 非法操作数会被提前碰到',
  },
  // ── `0x1a0` 的失败路径只写状态码 op1（★ 顺手写一格 op3 ⇒ "op3..9 保持原值"那条断言必须红）──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from:
      "  if (!host) throw new Error('save-host-missing:0x1a0');\n" +
      '  const slot = readInt(m, ops, 2) | 0;\n' +
      "  const bytes = await host.read(host.fileForSlot(slot, 'dat'));\n" +
      '  if (!bytes) {\n' +
      '    writeInt(m, ops, 1, 1);\n' +
      '    return;\n' +
      '  }',
    to:
      "  if (!host) throw new Error('save-host-missing:0x1a0');\n" +
      '  const slot = readInt(m, ops, 2) | 0;\n' +
      "  const bytes = await host.read(host.fileForSlot(slot, 'dat'));\n" +
      '  if (!bytes) {\n' +
      '    writeInt(m, ops, 1, 1);\n' +
      '    writeInt(m, ops, 3, 0);\n' +
      '    return;\n' +
      '  }',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '0x1a0 打不开时顺手写一格 op3 ⇒ 失败路径"op3..9 保持原值"的断言必须红（半截字段是静默的）',
  },
  // ── 0x1af 读侧的 BMP 判别（★ 容器解析已搬回 core：判据在 `core/bmp.ts` 的 `isBmp`）──
  //   ★ 判别写成别的字节后，真机 `.STH`（B=0x4d）会被当成"内容不可解"，而合成数据照样能过 ⇒
  //     "缩略图读得回来"这件事只有这条断言在守。
  {
    file: 'apps/emulator-new/src/core/bmp.ts',
    from: '  return bytes[0] === 0x42 && bytes[1] === 0x4d;',
    to: '  return bytes[0] === 0x42 && bytes[1] === 0x00; // 变异：判别字节写错',
    guard: 'tools/test/emulator-new-bmp.test.mjs',
    what: '0x1af 的 BMP 判别字节写错（0x4d → 0x00）⇒ 带 "BM" 的文件被判成不可解，状态码从 0 变 2',
  },
  // ── 0x1af：`.STH` 的**两段头读满**（★ 去掉长度检查 ⇒ 只剩 40 字节的文件走进位深 / 像素判据）──
  //   ★ 这条守的是**实现现状**（引擎只看 ReadFile 布尔、短读未取证 ⇒ 见 RES-SAVE-STH-SHORTREAD），
  //     所以它必须钉在实现守卫上（断言错误 code = bmp-header-short），⛔ 不能挪进 `spec-save-sth`。
  {
    file: 'apps/emulator-new/src/core/bmp.ts',
    from: '  if (bytes.length < HEADER_SIZE) {',
    to: '  if (bytes.length < 0) { // 变异：不再要求两段头读满',
    guard: 'tools/test/emulator-new-bmp.test.mjs',
    what: '0x1af 不再检查「两段头是否读满」⇒ 只剩 40 字节的 `.STH` 不再报 bmp-header-short（实现守卫必须红）',
  },
  // ── 写侧的**声明长度 = 行宽 × 高 + 40**（★ 改成 +54 就是"修正"那个写侧缺陷）──
  {
    file: 'apps/emulator-new/src/core/bmp.ts',
    from: '  writeU32LE(out, 2, pixelBytes + 40); // ★ 声明长度（刻意比实际短 14；见函数注释）',
    to: '  writeU32LE(out, 2, pixelBytes + 54); // 变异：把 +40 修正成 +54',
    guard: 'tools/test/emulator-new-bmp.test.mjs',
    what: '把 `.STH` 的声明长度"修正"成 54 + 像素 ⇒ `0x1ae` 写出的头不再是引擎的字节（容器守卫必须红）',
  },
  // ── 写侧的**行序自下而上**（★ 写成 `row` ⇒ 图上下颠倒，但文件仍然"能解"）──
  {
    file: 'apps/emulator-new/src/core/bmp.ts',
    from: '    let dst = HEADER_SIZE + (height - 1 - row) * rowStride;',
    to: '    let dst = HEADER_SIZE + row * rowStride; // 变异：行序写反',
    guard: 'tools/test/emulator-new-bmp.test.mjs',
    what: '`0x1ae` 写出时不再翻转行序（`height-1-row` → `row`）⇒ 缩略图上下颠倒而状态码仍是 0（行序守卫必须红）',
  },
  // ── `.STH` 的**声明长度 = 像素 + 40**（★ 改成 +54 就是"修正"那个写侧缺陷；真实样本守卫必须红）──
  {
    file: 'tools/test/spec-save-sth.assets.test.mjs',
    from: "  assert.equal(h.declaredSize, 40 + pixels, '声明的文件长度 = 像素字节数 + 40（不是 +54）');",
    to: "  assert.equal(h.declaredSize, 54 + pixels, '声明的文件长度 = 像素字节数 + 54'); // 变异：把 +40 修正成 +54",
    guard: 'tools/test/spec-save-sth.assets.test.mjs',
    what: '把 `.STH` 的声明长度"修正"成 54 + 像素 ⇒ 真机四份样本当场对不上（bfSize 是承重字段，不是可自由解释的怪癖）',
  },
  // ── 第三批残差（池翻页 / 键总数 / 帧辅助格 / AGF alpha / 图形参数 / 扩展索引 / 场景命令）──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (enabled === 0) {',
    to: '  if (false) { // 变异：不再按配置键拒绝',
    guard: 'tools/test/emulator-new-ops-memflip.test.mjs',
    what: '0x6d 去掉配置门 ⇒ 键缺失时仍翻页，而不是抛显示消息型异常（两份缓冲的不变量被绕过）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.globalFlip = m.globalFlip === 0 ? 1 : 0;',
    to: '  void 0; // 变异：翻页成 no-op',
    guard: 'tools/test/emulator-new-ops-memflip.test.mjs',
    what: '0x6d 不再切换主/备选择 ⇒ 翻页后仍读到原缓冲的 42',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '  get globalInt(): Map<number, number> { return this.intPoolAt(this.globalFlip); }',
    to: '  get globalInt(): Map<number, number> { return this.intPoolAt(0); } // 变异：两份缓冲塌成一份',
    guard: 'tools/test/emulator-new-ops-memflip.test.mjs',
    what: '全局整型池的 getter 固定 bank 0 ⇒ 翻页对读写不生效（看似实现了一半）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  const limit = readKeyTotal(m);',
    to: '  const limit = m.frame.keyLimit; // 变异：退回按帧上限',
    guard: 'tools/test/emulator-new-input-keytotal.test.mjs',
    what: '0x100 读 0xff 抄下的按帧上限而不是键总数 ⇒ 0xfe 与 0xff 的先后会改变派发结果',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '      localMain: -1, localSub: -1,',
    to: '      // 变异：两格不给初值',
    guard: 'tools/test/emulator-new-frame-aux.test.mjs',
    what: '新帧的两格本地块进入目标退回 undefined ⇒ "未登记"判断在 0 与 undefined 之间漂移',
  },
  {
    file: 'apps/emulator-new/src/core/agf.ts',
    from: '        rgba[d + 3] = body[s + 3]!;',
    to: '        rgba[d + 3] = alpha ? alpha[y * width + x]! : body[s + 3]!; // 变异：alpha 段覆盖 32bpp',
    guard: 'tools/test/emulator-new-agf.test.mjs',
    what: '32bpp 退回"ACIF alpha 覆盖像素第 4 字节" ⇒ 输出 alpha 从 10/20 变 200/0',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: '    case 3: return 4;',
    to: '    case 3: return 3; // 变异：3 直接透传',
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: 'PresentInterval 3 → 3（透传）⇒ 配置值与 D3D 档位值的映射被抹平',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: '    const fullscreenPresent = !((this.screenMode !== 0) || (virtualFullscreen !== 0));',
    to: '    const fullscreenPresent = this.screenMode === 0; // 变异：忽略虚拟全屏键',
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '是否窗口化只看形态字 ⇒ VirtualFullScreen 非 0 时仍请求全屏呈现',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: "    this.textureBitDepth = this.config.getInt('display:TextureBit');",
    to: "    this.textureBitDepth = this.config.getInt('display:FullScreenBit'); // 变异：读错键",
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '纹理位深读 display:FullScreenBit（旧式绘制路线的参数）⇒ 两条键的值互换时静默错',
  },
  {
    file: 'apps/emulator-new/src/core/install.ts',
    from: '    const zone = (id | 0) >> 24; // ★ 算术右移 = 有符号区号（128..255 得负数）',
    to: '    const zone = id >>> 24; // 变异：无符号区号',
    guard: 'tools/test/emulator-new-aai.test.mjs',
    what: '区号改成无符号右移 ⇒ 0x81… 落进槽表 129 而不是"未定义区号"',
  },
  {
    file: 'apps/emulator-new/src/core/install.ts',
    from: '    if (entry.offset === OFFSET_NO_ARCHIVE) throw new Error(`alf-no-archive:${entry.name}`);',
    to: '    if (false) throw new Error(`alf-no-archive:${entry.name}`); // 变异：不再看偏移哨兵',
    guard: 'tools/test/emulator-new-aai.test.mjs',
    what: 'offset == 0xFFFFFFFF 的哨兵不再挡住归档回退 ⇒ 取件会去读一个不存在的位置',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-order.ts',
    from: '    entries.push({ drawable: { kind: \'item\', key: item.key, item }, seq: index });',
    to: '    if (item.key < 0) return; // 变异：绘制项也过滤负键\n    entries.push({ drawable: { kind: \'item\', key: item.key, item }, seq: index });',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '绘制项恢复"负键不参与"⇒ spec 的按表分口径被抹成旧版通则（负键项永远不发出）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (split < 1) {',
    to: '  if (false) { // 变异：不检查分割幅',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '0x24f 不再检查分割幅 < 1 ⇒ 缺了"报告后仍照写（split=1 / mode=-1）"这条真实行为',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '    return; // ⛔ 不写表、⛔ 不删旧记录',
    to: '    m.scene.commands.delete(key); return; // 变异：拒绝前先删旧记录',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '0x24f 类型 > 11 时先删旧记录 ⇒ "表完全不变"这条合同被破坏（半截写）',
  },
  // ── 第四批：场景命令字段 / 纹理消费者规划器 / 图形重试与旧式路线 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  record.rect = { left, top, right: left + width, bottom: top + height };',
    to: '  record.rect = { left, top, right: width, bottom: height }; // 变异：右下不加上左',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '种类 1 的目标矩形右下没有由左 / 上加出来 ⇒ 覆盖区域少一块（看起来只是"矩形小了一点"）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  rampAngle = [angleFrom, angleTo];',
    to: '  rampCenterU = [angleFrom, angleTo]; // 变异：两变体共用布局',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '变体 0 的角度写进变体 1 的中心格 ⇒ 两种模糊的字段布局混在一起',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "    throw new Error('display-message: ムービーの初期化に失敗しました．\\r\\nテクスチャが確保されていません．');",
    to: '    void 0; // 变异：槽空不再抛',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '种类 1 的空纹理槽不再抛异常 ⇒ 会照常删旧记录 / 写记录（失败路径被吞掉）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.scene.errorBuffer = `関数：CTexture エラー：テクスチャ作成に失敗． TEXTURE=${textureId}`;',
    to: '  void 0; // 变异：失败不上报',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '种类 0 的建 / 换纹理失败不再上报 ⇒ 错误通道里没有带编号的说明（静默跳过）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '    timestamp: 0,',
    to: '    timestamp: 1, // 变异：写表时就盖了时间戳',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '命令记录的时间戳写表时不是 0 ⇒ 种类 1 / 3 的"第一帧"判据永远不会命中',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts',
    from: 'const ROUND_HALF_UP_BIAS = 0.5;',
    to: 'const ROUND_HALF_UP_BIAS = 0; // 变异：半上取整退化成截断',
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '位置取整丢 0.5 ⇒ 编号 20..29 + 缩放 2 + 位置 1.4 得到 2 而不是 3',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts',
    from: "  if (left >= right || top >= bottom) return finish('empty-clip', null, null);",
    to: "  if (left >= right || top >= bottom) return finish('empty-clip', { category: 'empty-clip', textureId, text: 'empty' }, null); // 变异：夹空也上报",
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '夹空从"静默"变成"上报" ⇒ 错误通道多出噪声（spec 明确夹空不上报）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts',
    from: `    return finish('missing-texture', {
      category: 'draw-source-missing',
      textureId,
      text: \`绘制源纹理尚未创建（纹理 \${textureId}）\`,
    }, null);`,
    to: "    return finish('missing-texture', null, null); // 变异：空槽静默",
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '空纹理槽不再上报 ⇒ 错误通道里没有带编号的说明（"画不出来"变成静默）',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: `  for (const deviceType of deviceTypeCandidates(deviceTypeKey)) {
    for (const vertexProcessing of vertexProcessingCandidates(vertexProcessingKey)) {
      attempts.push({ deviceType, vertexProcessing });
    }
  }`,
    to: `  for (const vertexProcessing of vertexProcessingCandidates(vertexProcessingKey)) {
    for (const deviceType of deviceTypeCandidates(deviceTypeKey)) {
      attempts.push({ deviceType, vertexProcessing });
    }
  } // 变异：两层循环互换`,
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '重试序列的层序互换 ⇒ 次序变成 (1,1)(2,1)(1,2)…（顶点处理不再内层）',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: '  return (useDirectDrawEmuration | (screenMode !== 0 ? 2 : 0)) >>> 0;',
    to: '  return useDirectDrawEmuration >>> 0; // 变异：丢掉窗口化位',
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '旧式行为位不再带"窗口化请求"位 ⇒ 窗口态被当成全屏',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: "        this.logGraphics('error', 'set-dialog-box-mode', GRAPHICS_DIALOG_BOX_MODE_ERROR_TEXT);",
    to: "        this.failGraphics('set-dialog-box-mode'); // 变异：错误文本当致命",
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '对话框绘制模式失败改成结束启动 ⇒ 与"错误文本 ≠ 致命"的逐步骤表冲突',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: '    if (threeDimensional) {',
    to: '    if (false && threeDimensional) { // 变异：三维路线被跳过',
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '三维位被短路 ⇒ 键 = 3 时只跑旧式路线（两个独立位退化成 if/else 的一种表现）',
  },
  // ── 第五批：分辨率判据 / 日志落点 / 源矩形窗口 / 场景自身变换 ──
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: `    } else if (ratio > R) {
      if (wide !== null && (wide.width < mode.width || wide.height < mode.height)) entry.symbol = '―';
      else { wide = entry; entry.symbol = '△'; }`,
    to: `    } else if (ratio > R) {
      if (wide !== null && (wide.width < mode.width && wide.height < mode.height)) entry.symbol = '―';
      else { wide = entry; entry.symbol = '△'; } // 变异：池替换的"任一更小"改成"两者都更小"`,
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '池替换判据从"宽或高任一更小"改成"两者都更小" ⇒ 该替换的档不替换（池里留下更大的档）',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: '    } else if (ratio >= RESOLUTION_ASPECT_THRESHOLD) {',
    to: '    } else if (ratio > RESOLUTION_ASPECT_THRESHOLD) { // 变异：r = 0.7 落错池',
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '阈值 `>=` 改 `>` ⇒ r 恰好 0.7 的档从窄档池掉进宽档池（符号与收口都可能翻）',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: '    if (width !== 0 || height !== 0) return { width, height };',
    to: '    if (width !== 0 || height !== 0) return { width: width !== 0 ? width : this.clientWidth(), height: height !== 0 ? height : this.clientHeight() }; // 变异：逐分量回退',
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '请求对退回逐分量回退 ⇒ FullScreenWidth=0 / Height=1080 时宽被换成客户区宽',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: "    this.logSink = opened ? 'file' : 'unavailable';",
    to: "    this.logSink = 'file'; // 变异：记录 = 送达",
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '日志落点恒 file ⇒ 打开失败时仍声称"送达"（记录 ≠ 送达被抹掉）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts',
    from: '      const expired = input.forcedComplete === 1 || (now >>> 0) > deadline; // 相等算窗内（70-vm）',
    to: '      const expired = input.forcedComplete === 1 || (now >>> 0) >= deadline; // 变异：相等就算到期',
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '到期边界改回"相等也到期" ⇒ 与「场景计时窗」的"相等算窗内"冲突',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts',
    from: '        else rect = shiftRect(rect, delta);',
    to: '        else { rect = shiftRect(rect, delta); rectWriteBack = { ...rect }; } // 变异：推进中也写回',
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '推进中也写回矩形 ⇒ 记录逐帧漂走（只有到期帧才回写）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts',
    from: '  return value >= start && value < end;',
    to: '  return value >= start && value <= end; // 变异：右端改闭',
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '键区间右端改闭 ⇒ key = 起点+计数 也被算进作用区间',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: `  item.flags |= 2;
  item.start = 0;
  setField(item, 0x34, 0);
  setField(item, 0x48, a);`,
    to: `  item.flags |= 2;
  void 0; // 变异：不清共用窗起点
  setField(item, 0x34, 0);
  setField(item, 0x48, a);`,
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '0x239 不清共用窗起点 ⇒ 新的源矩形窗沿用上一次的起点（第一帧判据错）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.scene.selfRecord.keyStart = start;',
    to: '  void start; // 变异：不写区间起点',
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '0x229 不写区间起点 ⇒ 作用区间恒从 0 开始（额外变换错位）',
  },
  // ── 第六批：0x199 本地块 / 种类 2 / 日志路径 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.localSavePos = m.ip / 4 + 1;',
    to: '  m.localSavePos = m.ip / 4; // 变异：返回点少 1（回到本条自己）',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '0x199 首次进入的返回点少 1 ⇒ 0x7c 回到 0x199 自身（死循环）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  } else if (mode < 0 || mode > 11) {',
    to: '  } else if (mode > 11) { // 变异：负数放行',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '种类 2 类型判据只挡 > 11 ⇒ −1 被写进表（与"两端闭、含负数"相反）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '    delay = 0; // ★ 清的是**实参**：记录里落的也是 0（不是"反正默认 0 就不写"）',
    to: '    delay = readInt(m, ops, 9); // 变异：不清延迟',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '分割幅 < 1 支不清延迟 ⇒ 记录里留着原延迟（spec 要求清 0 后照写）',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    localDepth: id ?? -1,',
    to: '    localDepth: -1, // 变异：不取帧身份',
    guard: 'tools/test/emulator-new-ops-100-200.test.mjs',
    what: '建帧不写身份编号 ⇒ 0x199 记下的入块深度恒 −1，0x7c 的深度判据与真实帧身份脱钩',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: `  path += parts.relativeDirectory;
  if (path !== '') path += ENGINE_PATH_SEPARATOR;`,
    to: `  path += parts.relativeDirectory;
  if (parts.relativeDirectory !== '') path += ENGINE_PATH_SEPARATOR; // 变异：看目录是否为空，而不是看拼到此处的结果`,
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: '第二个分隔符只在"相对目录非空"时补 ⇒ 根非空+目录空时丢掉双分隔符（与引擎路径规则相反）',
  },
  {
    file: 'apps/emulator-new/src/core/window.ts',
    from: "    const useAppDataFolder = this.readConfigInt('set:UseAppDataFolder') === 1;",
    to: "    const useAppDataFolder = this.readConfigInt('set:UseAppDataFolder') !== 0; // 变异：非 0 就当 1",
    guard: 'tools/test/emulator-new-graphics-params.test.mjs',
    what: 'UseAppDataFolder 判"非 0"⇒ 值 2 也走用户目录（spec 是"恰为 1"）',
  },
  // ── 第七批：按住派发按键族 ──
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  if (key < 0 || key > 31) throw new Error('display-message: SetKeyJumpEx の第１引数が不正です（键编号越界）');",
    to: "  if (key < 0 || key > 32) throw new Error('display-message: SetKeyJumpEx の第１引数が不正です（键编号越界）'); // 变异：键上界放宽到 32",
    guard: 'tools/test/emulator-new-input-hold.test.mjs',
    what: '0x102 键编号上界从 31 放宽到 32 ⇒ 键 32 也被写进表（spec：0..31）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: `  for (let i = limit - 1; i >= 0; i -= 1) {
    if (((word >>> i) & 1) === 1) { key = i; break; }
  }`,
    to: `  for (let i = 0; i < limit; i += 1) { // 变异：反向改正向
    if (((word >>> i) & 1) === 1) { key = i; break; }
  }`,
    guard: 'tools/test/emulator-new-input-hold.test.mjs',
    what: '0x104 扫描改成正向 ⇒ 同时按住 3 与 7 时先派发 3（spec：先派发最高位 7）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.hold.modulus = 0;',
    to: '  void 0; // 变异：0xfc 不复位模数',
    guard: 'tools/test/emulator-new-input-hold.test.mjs',
    what: '0xfc 不清模数 ⇒ 复位后仍沿用旧口径（spec：模数 ← 0，下一次 0x104 应响亮失败）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  const modulus = m.hold.modulus | 0;',
    to: '  const modulus = (m.hold.modulus | 0) || 1; // 变异：模数 0 兜底成 1',
    guard: 'tools/test/emulator-new-input-hold.test.mjs',
    what: '模数 0 兜底成 1 ⇒ 把"未定义行为"伪装成可执行（spec 要求显式响亮失败）',
  },
  // ── 第八批：显示适配 / 矩阵门 ──
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts',
    from: '    if (target.id < 0 || target.id === RESERVED_VIEW_TARGET) {',
    to: '    if (target.id >= 0 && target.id !== RESERVED_VIEW_TARGET) { // 变异：旧的反门',
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '视图矩阵门写成"目标 ≥ 0 且 ≠ 39"（反门）⇒ 目标 38 与纹理目标被乘、后缓冲那路漏乘',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/display-fit.ts',
    from: '    CW = Math.trunc(BH * (1 / t));',
    to: '    CW = Math.trunc(BH / t); // 变异：先除再截断（边界差 1 像素）',
    guard: 'tools/test/emulator-new-display-fit.test.mjs',
    what: '画布宽用 `BH/t` 而不是 `BH*(1/t)` ⇒ 边界用例得到 1921 而不是 1920',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/display-fit.ts',
    from: '  return Math.trunc(x * 0.5);',
    to: '  return Math.floor(x * 0.5); // 变异：向零截断变向下取整',
    guard: 'tools/test/emulator-new-display-fit.test.mjs',
    what: 'half 用 floor ⇒ 负奇数差得到 −12 而不是 −11（原点偏 1 像素）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts',
    from: '  const fixedResolution = view?.fixedResolution === true;',
    to: '  const fixedResolution = false; // 变异：固定分辨率模式失效',
    guard: 'tools/test/emulator-new-scene-draw-plan.test.mjs',
    what: '固定分辨率模式不再短路缩放 / 原点 ⇒ 该模式下位置仍被乘缩放',
  },
  // ── 第九批：命令档规划器 ──
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts',
    from: '  const belowRangeAllowed = tier < 2;',
    to: '  const belowRangeAllowed = tier <= 2; // 变异：能力档 2 也做区间之下',
    guard: 'tools/test/emulator-new-scene-command-plan.test.mjs',
    what: '能力档 2 仍做「区间之下」⇒ 该档本应完全抑制这一趟',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts',
    from: '  const clearTable = animating === 0 && forcedComplete === 0;',
    to: '  const clearTable = animating === 1; // 变异：清表判据写反',
    guard: 'tools/test/emulator-new-scene-command-plan.test.mjs',
    what: '清表判据写成"有动画才清" ⇒ 还在动的命令被清、静止的反而留下',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts',
    from: '    a.commandKey < b.commandKey ? -1 : a.commandKey > b.commandKey ? 1 : 0',
    to: '    a.commandKey < b.commandKey ? 1 : a.commandKey > b.commandKey ? -1 : 0 // 变异：降序',
    guard: 'tools/test/emulator-new-scene-command-plan.test.mjs',
    what: '命令档按命令键降序执行 ⇒ 与"严格升序"相反',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts',
    from: '    if (timestamp === 0) {',
    to: '    if (false) { // 变异：不盖时间戳',
    guard: 'tools/test/emulator-new-scene-command-plan.test.mjs',
    what: '首次遍历不盖时间戳 ⇒ 种类 1 的"第一帧"判据永远不命中',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts',
    from: '    if (other !== null && entry.key >= other.start && entry.key < other.start + other.count) continue;',
    to: '    if (false) continue; // 变异：不跳过另一段',
    guard: 'tools/test/emulator-new-scene-command-plan.test.mjs',
    what: '双画布种类不跳过另一段区间 ⇒ 两个画布内容互相污染',
  },
  // ── 第十批：12 抹除图案 ──
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/wipe-pattern.ts',
    from: '  const phase = mode === 0 || mode === 2 ? 0 : 1;',
    to: '  const phase = 0; // 变异：1/3 的相位 +1 丢失',
    guard: 'tools/test/emulator-new-wipe-pattern.test.mjs',
    what: '模式 1/3 的步序不再 +1 ⇒ 它们与 0/2 同相（少一道）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/wipe-pattern.ts',
    from: '  const reveal = clamp(staggered ? index - stripe : index, 0, split);',
    to: '  const reveal = staggered ? index - stripe : index; // 变异：不钳在本带内',
    guard: 'tools/test/emulator-new-wipe-pattern.test.mjs',
    what: '条带让出量不钳在本带内 ⇒ 步序超过 S 后每带继续变宽（越出本带）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/wipe-pattern.ts',
    from: '      : axisEnd - remainder - split * stripe;',
    to: '      : axisEnd - split * stripe; // 变异：反向网格丢掉余数',
    guard: 'tools/test/emulator-new-wipe-pattern.test.mjs',
    what: '反向族的第 0 带不再按余数锚定 ⇒ 网格整体偏移（越界量错）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/wipe-pattern.ts',
    from: "  if (!(duration > 0)) diagnostics.push('wipe-duration-zero');",
    to: "  if (!(duration > 0)) { /* 变异：不记诊断 */ }",
    guard: 'tools/test/emulator-new-wipe-pattern.test.mjs',
    what: 'duration = 0 不再记诊断 ⇒ "保持未定"被静默吞掉',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/wipe-pattern.ts',
    from: '    { canvas: CANVAS_A, rect: first },',
    to: '    { canvas: CANVAS_B, rect: first }, // 变异：A/B 颠倒',
    guard: 'tools/test/emulator-new-wipe-pattern.test.mjs',
    what: '两笔的落笔顺序/画布颠倒 ⇒ 先 B 后 A（与"先 A 后 B"相反）',
  },
  // ── 第十一批：0x1a1 读回方向 ──
  // ── 第十二批：抹除笔接线 ──
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts',
    from: '    elapsed: now - (timestamp + command.delay),',
    to: '    elapsed: now - timestamp, // 变异：不减延迟',
    guard: 'tools/test/emulator-new-scene-command-plan.test.mjs',
    what: '抹除笔的经过时间不减延迟 ⇒ 有延迟的命令提前开始（笔的进度整体偏移）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts',
    from: "    note('scene-command-wipe-rect-unmodeled', command.commandKey);\n    return null;",
    to: "    return []; // 变异：缺矩形当'算过、没笔'",
    guard: 'tools/test/emulator-new-scene-command-plan.test.mjs',
    what: '缺矩形不再记缺口、返回空笔 ⇒ 把"算不出来"伪装成"这一帧没笔"',
  },
  // ── 第十四批：存档块布局 ──
  {
    file: 'apps/emulator-new/src/core/save-codec.ts',
    from: "  if (magic !== 'S4SD' && magic !== 'S3SD') throw new SaveFormatError('save-magic');",
    to: '  // 变异：不校验魔数',
    guard: 'tools/test/emulator-new-save-codec.test.mjs',
    what: '读侧不校验魔数 ⇒ 任意文件都能进负载解析（把别的游戏存档当本作）',
  },
  {
    file: 'apps/emulator-new/src/core/save-codec.ts',
    from: '  const textureBytes = version1 === 3 ? 20000 : 0;',
    to: '  const textureBytes = 0; // 变异：v1=3 也不认 20000 B 纹理表',
    guard: 'tools/test/emulator-new-save-codec.test.mjs',
    what: '读侧不认 v1=3 的 20000 B 纹理表 ⇒ 帧记录整体前移 20000 B（记录的 parent 读成纹理数据）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "      m.diagnostics.push({ kind: 'save-1a1-parse-' + code, index: slot });\n      return;",
    to: "      m.diagnostics.push({ kind: 'save-1a1-parse-' + code, index: slot }); // 变异：失败也继续还原",
    guard: 'tools/test/emulator-new-save-1a1.test.mjs',
    what: '0x1a1 解析失败后仍进“还原未建模”支 ⇒ 坏档被当成读成功（不写 op1 但假装恢复）',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    bank.int.clear();',
    to: '    // 变异：不清整型池',
    guard: 'tools/test/emulator-new-save-1a1.test.mjs',
    what: '还原前不清整型池 ⇒ 文件里没有的槽保留旧值（存档污染当前状态）',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '      if (rec.tableB >= 0 && tables?.call?.[rec.tableB] !== undefined) ip = tables.call[rec.tableB]! * 4 + 12;',
    to: '      if (false) ip = 0; // 变异：忽略 tableB 优先级',
    guard: 'tools/test/emulator-new-save-1a1.test.mjs',
    what: '还原执行位置不优先 tableB ⇒ 换脚本位置不 +12 / 落回 tableA（恢复后重跑调用）',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    for (let i = 0; i < pools.ints.length; i += 1) bank.int.set(i, encodeInt(pools.ints[i]! | 0, this.key));',
    to: '    for (let i = 0; i < pools.ints.length; i += 1) bank.int.set(i, pools.ints[i]! | 0); // 变异：不重编码',
    guard: 'tools/test/emulator-new-save-1a1.test.mjs',
    what: '还原整型池不按当前 key 重编码 ⇒ 脚本读到错值（存档来自别的 key）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-order.ts',
    from: '    if (!Number.isInteger(value) || value < 0 || value > 3) continue;',
    to: '    // 变异：不过滤种类',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: 'URL 开闸不过滤种类 ⇒ 任意数字 / 负数被当成命令种类（未定义行为被静默放行）',
  },

  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  if (m.hold.contextKey !== index) m.holdCursorBySlot[m.cur] = 0;',
    to: '  m.holdCursorBySlot[m.cur] = 0; // 变异：无条件清',
    guard: 'tools/test/emulator-new-input-hold.test.mjs',
    what: '上下文闸无条件清当前槽游标 ⇒ 帧释放/重载后的定义值被抹掉（残值路径变错）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.holdCursorBySlot[m.cur] = (cursor + 1) % modulus;',
    to: '  m.holdCursorBySlot[m.cur] = (((cursor + 1) % modulus) + modulus) % modulus; // 变异：数学取模',
    guard: 'tools/test/emulator-new-input-hold.test.mjs',
    what: '负模数改成数学取模 ⇒ 与 idiv 的余数符号相反（−7 的序列变错）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts',
    from: 'const MERGE_AUX_A = 0;',
    to: 'const MERGE_AUX_A = 2; // 变异：辅助表 A 排到绘制项之后',
    guard: 'tools/test/emulator-new-scene-command-plan.test.mjs',
    what: '合并序把辅助表 A 排到网格/绘制项之后 ⇒ 同键辅助表 A 被画晚（引擎比较链是 A→网格→绘制项）',
  },
  {
    file: 'apps/emulator-new/src/core/save-codec.ts',
    from: '      const off = textureBase + 20 * i;',
    to: '      const off = textureBase + 5 * i; // 变异：项步长写成 5',
    guard: 'tools/test/emulator-new-save-textures.test.mjs',
    what: '纹理记录表项步长 20→5 ⇒ 只有第 0 项对、其后全部错位（恢复只看第 0 项）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '      flag0: m.scene.flagTableA[id] ?? 0,\n      flag1: m.scene.flagTableB[id] ?? 0,',
    to: '      flag0: m.scene.flagTableB[id] ?? 0, // 变异：门与零读者标志互换\n      flag1: m.scene.flagTableA[id] ?? 0,',
    guard: 'tools/test/emulator-new-save-textures.test.mjs',
    what: '纹理记录表的恢复门与零读者标志互换 ⇒ 恢复按错的那一格开门（纹理重建判据反了）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: '  if (input.enabled !== true) return { ops: [], diagnostics: [] };',
    to: '  if (input.enabled === false) return { ops: [], diagnostics: [] }; // 变异：默认变开启',
    guard: 'tools/test/emulator-new-scene-command-exec.test.mjs',
    what: '执行器总闸默认开启 ⇒ 未接线的产品路径也会产 op（零回退破坏）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: "  ops.push({ kind: 'copy', from: 'canvasA', to: 'own', commandKey: planned.commandKey });",
    to: "  ops.push({ kind: 'copy', from: 'own', to: 'canvasA', commandKey: planned.commandKey }); // 变异：方向反",
    guard: 'tools/test/emulator-new-scene-command-exec.test.mjs',
    what: '种类 1 的表面拷贝方向反了 ⇒ 把命令纹理拷回画布（spec 明禁的方向）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: "  ops.push({ kind: 'clear', target: canvas, commandKey });",
    to: "  ops.push({ kind: 'clear', target: 'own', commandKey }); // 变异：清错目标",
    guard: 'tools/test/emulator-new-scene-command-exec.test.mjs',
    what: '画布用前清错目标 ⇒ 画布没清、命令自己的纹理反被清（残留 / 同帧两次）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-order.ts',
    from: '  if (kinds.length === 0) return { ...plan, commands: [] };',
    to: '  if (kinds.length === 0) return plan; // 变异：空闸门放行',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '种类闸门为空时仍放行命令 ⇒ 产品默认路径产 op（零回退破坏）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: "  ops.push({ kind: 'copy', from: 'canvasA', to: 'own', commandKey: planned.commandKey });",
    to: "  ops.push({ kind: 'copy', from: 'own', to: 'canvasA', commandKey: planned.commandKey }); // 变异：方向反",
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '种类 1 copy 方向反了 ⇒ 命令纹理拷回画布（spec 明禁）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: "    sources: ['canvasA', 'canvasB'],",
    to: "    sources: ['canvasB', 'canvasA'], // 变异：合成顺序反",
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '双画布合成顺序反了 ⇒ B 盖 A（与"先 A 后 B"相反）',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '      if (record.timestamp !== 0) continue;',
    to: '      if (false) continue; // 变异：每帧都盖',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '时间戳每帧都盖 ⇒ 命令永远停在"第一帧"（多帧动画不推进）',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '    if (this.scene.animating === 0) this.scene.commands.clear();',
    to: '    this.scene.commands.clear(); // 变异：不管有动画都清',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '收尾不看有动画就清命令表 ⇒ 多帧命令只活一帧',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: '    progress: source.progress,',
    to: '    progress: 1, // 变异：退路合成恒不透明',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '种类 0 退路合成进度恒 1 ⇒ 第二块画布不按进度淡入（直接显示终态）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-order.ts',
    from: '  return (now - start) / command.duration;',
    to: '  return 1; // 变异：进行中也当到期',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '进行中的进度直接算 1 ⇒ 所有淡入在第一帧就完成',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-order.ts',
    from: '  if (command.kind === 3) {',
    to: '  if (false) { // 变异：种类 3 也走种类 0 的夹取',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '种类 3 的进度被当成种类 0（未开始夹成 0）⇒ 模糊转场在延迟内不按负进度外推',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: "        record.duration = 0;\n        continue;",
    to: "        continue; // 变异：到期不清 duration",
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '到期不清 duration ⇒ 进度判据永远停在"进行中"（动画不结束）',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: "      if (record.kind === 2 && record.mode < 0) {\n        this.scene.forcedComplete = 1;",
    to: "      if (false) {\n        this.scene.forcedComplete = 1; // 变异：不置强制完成",
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '种类 2 的 mode<0 不置强制完成 ⇒ 错误记录的唯一可见后果消失',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: '  if (planned.kind === KIND_WIPE && strokes.length > 0) {',
    to: '  if (false) { // 变异：有笔也不用，退回 composite',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '种类 2 有抹除笔时仍用 composite ⇒ 图案被"整块合成"冒充（笔序 / 分区消失）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: '  if (source.rect !== undefined) {',
    to: '  if (false) { // 变异：种类 1 不做遮罩',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '种类 1 不产 mask op ⇒ 影片遮罩混合消失（每帧只剩首帧 copy）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: '    const coeff = mask === 255 ? 254 / 255 : 0;',
    to: '    const coeff = mask === 0 ? 254 / 255 : 0; // 变异：遮罩系数取反',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '遮罩系数取反 ⇒ 该显示的一帧透明、该透的一帧显示',
  },
  {
    file: 'apps/emulator-new/src/core/machine.ts',
    from: '        if (record.duration > 0 && !this.scene.objectSlots.has(record.textureId)) {',
    to: '        if (false) { // 变异：影片结束不补遮罩',
    guard: 'tools/test/emulator-new-scene-commands.test.mjs',
    what: '影片结束不补遮罩反值 ⇒ 遮罩停在写表值（该透的帧仍显示）',
  },
  {
    file: 'apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts',
    from: "  if (planned.kind === KIND_TEXTURE_OVERLAY) note('scene-command-effect-unmodeled', planned.commandKey);",
    to: '  // 变异：不记效果未建模',
    guard: 'tools/test/emulator-new-scene-command-run.test.mjs',
    what: '种类 0 走退路却不记 effect-unmodeled ⇒ 缺口静默（看起来像已经支持效果技法）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: '  m.cur = slot;\n  let body: Uint8Array | null;',
    to: '  m.cur = caller;\n  let body: Uint8Array | null; // 变异：打开失败不切帧',
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '0x6 打开失败不切到目标槽 ⇒ 重试落在调用者帧（与 spec 相反）',
  },
  {
    file: 'apps/emulator-new/src/core/handlers.ts',
    from: "  if (!body) throw new FrameLoadError(7, 'load-frame-open-failed:' + id);",
    to: "  if (!body) throw new FrameLoadError(1, 'load-frame-open-failed:' + id); // 变异：当成退出型",
    guard: 'tools/test/emulator-new-frame.test.mjs',
    what: '0x6 打开失败抛退出型 ⇒ 可重试路径消失（只能终止）',
  },
  {
    file: 'apps/emulator-cxx/third_party/pl_mpeg/pl_mpeg.h',
    from: 'PL_MPEG - MPEG1 Video decoder, MP2 Audio decoder, MPEG-PS demuxer',
    to: 'PL_MPEG - MPEG1 Video decoder, MP2 Audio decoder, MPEG-PS demuxer (drifted)',
    guard: 'tools/test/emulator-cxx-pl-mpeg-sync.test.mjs',
    what: '只改 C++ 那份 pl_mpeg 副本 ⇒ 两处解码器漂移（两边的影片测试各自仍然绿）',
  },
  // ── C++ 分派表 ↔ handler 声明（★ 删掉一行 `case` ⇒ handler 还在、编译链接全绿，
  //    但该 opcode 运行期落进 default 抛"未实现" —— 这正是"看起来实现了、其实没接上"）──
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    from: '        case 0xA3: op_a3(instruction); return;\n',
    to: '',
    guard: 'tools/test/emulator-cxx-dispatch.test.mjs',
    what: '`0xa3` 的 case 被删掉（声明与定义都在）⇒ 分派表守卫当场红（否则"补了指令"只是补了函数体）',
  },
  // ── wait-subagent 插件：`any_of` 必须如实交回“谁还在跑” ──
  //   ★ 把 `pending` 抹成 `[]` ⇒ 模型以为“都结束了”，于是不再等剩下的 completion notice。
  {
    file: 'plugins/wait-subagent/index.mjs',
    from: 'pending: targets.map((target) => target.childId),',
    to: 'pending: [],',
    guard: 'plugins/wait-subagent/test/plugin.test.mjs',
    what: 'any_of 的 already_finished 不再列出仍在跑的 child ⇒ “还有人没结束”被当成“都结束了”',
  },
  // ── wait-subagent 插件：同一同步边上“人消息优先于 end” ──
  //   ★ 拿掉这条 ⇒ 人正在等的那条插话被 end 吞掉（等待返回“finished”，而人消息没人看）。
  {
    file: 'plugins/wait-subagent/lib/wait.mjs',
    from:
      '      const pending = pendingHumanMessages(agent);\n' +
      '      if (pending.length > 0) {\n' +
      '        interjected(pending[0]);\n' +
      '        return;\n' +
      '      }',
    to:
      '      const pending = pendingHumanMessages(agent);\n' +
      '      if (false && pending.length > 0) {\n' +
      '        interjected(pending[0]);\n' +
      '        return;\n' +
      '      }',
    guard: 'plugins/wait-subagent/test/wait.test.mjs',
    what: 'end 与人消息同一同步边到达时不再优先报人插话 ⇒ 人正在等的那条消息被吞掉',
  },
  // ── fixtures：**一个文件只能有一个身份**（槽载荷 vs 系统容器）──
  //   ★ 去掉它 ⇒ 同一份 `SAVE76.DAT` 能同时登记成槽载荷与 `containers[]`，
  //     两条不变量会互相打架，而 `--list` / 守卫都看不出来。
  {
    file: 'tools/lib/samples.mjs',
    from: '    if (payloadNames.has(n)) bad.push(`containers 的 ${n} 与槽载荷重名（一个文件只能有一个身份）`);',
    to: "    if (false) bad.push('x');",
    guard: 'tools/test/fixtures-cli-write.test.mjs',
    what: '去掉"一个文件只能有一个身份" ⇒ 同一份文件能同时登记成槽载荷与系统容器',
  },
  // ── spec 证据分级：CTest 的档位看**注册位置**（顶层 = pure，块里 = assets）──
  //   ★ 把它退化成"一律 pure" ⇒ 装机流程里的 CTest（`if(EMULATOR_CXX_INSTALL_DIR)` 里注册的那些）
  //     会被当成"默认门禁就会跑"，于是**默认门禁根本不跑的用例**撑起了 reviewed。
  {
    file: 'tools/lib/ledger.mjs',
    from: "    if (hit) return depth > 0 ? 'assets' : 'pure';",
    to: "    if (hit) return 'pure';",
    guard: 'tools/test/spec.test.mjs',
    what: 'CTest 的档位退化成"一律 pure" ⇒ 装机流程里的用例被当成默认门禁会跑，撑起了 reviewed',
  },
  // ── 音效槽：引擎表 **10** 格 vs 后端 **15** 个槽（★ 合成一个 15 ⇒ 凭空多出 5 个引擎格）──
  //   ★ 配套的 C++ 侧守护在 `emulator-core-selftest`（`check_audio_slot_table` / `check_audio_slot_bounds`）；
  //     这条变异打的是**跨源判据**（shape ↔ 实现 ↔ spec 的 storage 指针）。
  {
    file: 'apps/emulator-cxx/src/core/vm.h',
    from: '    static constexpr int kSeSlotTableSize = 10;',
    to: '    static constexpr int kSeSlotTableSize = 15;',
    guard: 'tools/test/emulator-cxx-audio-slots.test.mjs',
    what: '引擎的槽表量写成后端的 15 ⇒ "槽号 10..14 装着什么"变成可读，而引擎里根本没有那 5 格',
  },
  // ── 注释里引用的规范条目 id 必须存在（★ 实测两次：`0xb6` 引了一条不存在的 `CORE-OPS-AUDIO-SE-STOP`，
  //    并据此写下了"规范：不清"的断言；帧族把 `CORE-OPS-FRAME-*` 写成 `CORE-FRAME-*`）──
  // ── 复审修复的源码锚（行为断言在 CTest；这里给变异测试一个 node 可跑的入口）──
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_text.cpp',
    from: 'msg_arm_ |= 0x10000;',
    to: '',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x6e / 0x196 少一处置 arm bit16 ⇒ 0x305 的整段块结束逻辑重新变成死代码',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_scene.cpp',
    from: 'std::swap(scene_records_[key_a], scene_records_[key_b]);',
    to: '/* mutated */;',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x214 两边都有记录时不再交换 ⇒ 只剩单向覆盖（源记录被删）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_scene.cpp',
    from: 'msg_wait_c_ = value;',
    to: '(void)value;',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x238 不再写消息等待三格的 C ⇒ 与 0x243 / 0x305 的三格脱钩',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    from: 'item.set21e_int0 = g1;',
    to: 'item.set21e_int0 = 0;',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x220 的 G1 不再同步到 DrawItem ⇒ 窗位/配置格对消费者不可见',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_text.cpp',
    from: 'if (!rec.key_base_ready()) return;',
    to: 'if (!rec.key_base_set) return;',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '绘制条目只看“脚本写过 0x213”⇒ 没有 0x213 的 ADV 消息层一条绘制条目都不建（文字整段消失）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    from: 'const bool shrink = scale_x >= 1.0 && scale_y >= 1.0;',
    to: 'const bool shrink = false;',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x32 不再区分缩小支 ⇒ 缩小退回抽点/单点采样（面积平均没有出口）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    from: 'if (msg_wait_c_ != 0) {',
    to: 'if (false) {',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x21c 的等待位退回「场景无动画才放行」⇒ 每句 1800ms 淡入把脚本卡住、点击全丢（实测 80 击 13 推进）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_io.cpp',
    from: 'choice_window_.fallback = -1;',
    to: '/* mutated */;',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x93 清表漏掉兜底跳转目标 ⇒ 上一个选项窗的兜底目标漏到下一个窗',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    from: 'if (down && !advance_input_prev_) advance_input_latch_ = true;',
    to: 'if (down) advance_input_latch_ = true;',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x72 的等推进退回“按电平置锁存”⇒ 按住不放会一句接一句推进（实测回归）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    from: 'const auto mask = read_int(ops[1]);',
    to: 'const auto mask = read_int(ops[0]);',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x258 读序从 2→1 退回 1→2（两个操作数都非法时抛点不同）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_engine.cpp',
    from: 'config_.set_int("message:ReadTextSkip", read_int(ops[0]));',
    to: 'config_.set_int("message:ReadTextSkip", 0);',
    guard: 'tools/test/emulator-cxx-review-regressions.test.mjs',
    what: '0x1ca 不再把操作数写进命名键 ⇒ 0x1cb 的 falsifier“设 1 再读必须是 1”失败',
  },
  // ── spec 证据锚的“覆盖”判据（★ 2026-10-30 复审实测：19 条新规则引用站点用例，
  //    而该用例的 RULES 名单一条都不含它们 ⇒ 站点写错/删空也不会红）──
  {
    file: 'tools/test/spec-ops-100-200-sites.assets.test.mjs',
    from: "  'CORE-OPS-MSG-WAIT-INPUT-72',\n",
    to: '',
    guard: 'tools/test/spec-evidence-covers-rule.test.mjs',
    what: '站点用例的 RULES 名单删掉一条仍被 bindings 引用 ⇒ “用例存在”被当成“用例覆盖该 rule”',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_engine.cpp',
    from: '规则 `CORE-OPS-AUDIO-WAV`）',
    to: '规则 `CORE-OPS-AUDIO-WAV-SLOT`）',
    guard: 'tools/test/spec-rule-refs.test.mjs',
    what: '注释指向一条不存在的规范条目 ⇒ 读者把"注释说有这条规范"当成证据',
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
      // ★ 这不是"守卫没抓住"，而是**清单过期**：`from` 已经不在源文件里 ⇒ 这次自检什么也没验到。
      //   必须与"守卫没红"分开报 —— 否则真实症状（清单没跟上源文件）会被读成"守卫是假的"。
      results.push({ ...m, verdict: '⚠ 变异点没找到（源文件已变 ⇒ 清单过期）', ok: false, stale: true });
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
  const stale = results.filter((r) => r.stale);
  const failed = results.filter((r) => !r.ok && !r.unverifiable && !r.stale);
  const unver = results.filter((r) => r.unverifiable);
  console.log(`\n${results.length - failed.length - unver.length - stale.length}/${results.length} 条按预期变红${unver.length ? ` · ${unver.length} 条**无法判定**（守卫在本机 skip ⇒ 不能算"没红"）` : ''}。`);
  if (unver.length) {
    console.log('⏭ 这些守卫在**本机**跑不起来（不是"没抓住"）—— 要它有意义就得先备齐它的语料/资产：');
    for (const u of unver) console.log(`  - ${u.what}（${u.guard}）：${u.ran.skipReason ?? '（无原因）'}`);
  }
  if (stale.length) {
    console.error('⚠ 这些变异的**施加点已经不存在**（源文件改过 ⇒ 清单过期，等于什么也没验到）：');
    for (const s of stale) {
      console.error(`  - ${s.file}：找不到 ${JSON.stringify(s.from)}（本该由 ${s.guard} 抓）`);
    }
  }
  if (failed.length) {
    console.error('★ 这些守卫**没抓住**已知的破坏 ⇒ 它们不是"会红的守卫"（断言恒真？或压根没验那个常量）：');
    for (const f of failed) console.error(`  - ${f.what}（${f.guard}）`);
    return 1;
  }
  return stale.length ? 1 : 0;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) process.exitCode = main();

