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
    from: "    out.set(`${section}\\\\${trimField(raw.slice(0, eq))}`, trimField(raw.slice(eq + 1)));",
    to: "    out.set(`${section}:${trimField(raw.slice(0, eq))}`, trimField(raw.slice(eq + 1))); // 变异：段键分隔符写错",
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
    from: '    return this.index.files[id];',
    to: '    return this.index.files[id + 1] ?? this.index.files[id]; // 变异：id 偏一位',
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

