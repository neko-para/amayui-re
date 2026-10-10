/** @env assets @kind gate @why 帧记录基址被改回旧值、记录大小被改坏，或有人把记录之外的 Engine 级标量当成 per-cur 记录字段读 */
/**
 * tools/test/engine-frame.test.mjs —— 帧布局的守卫（**只算布局，不取值**）
 *
 * ## ★★ 2026-10 三次订正：**记录基址 = `0x5D894`、记录大小 = 索引步长 = `0x78`**
 * 台账 `KN-01M4H0MZGD4K4P4F0J1E7E5G79`（`replaces` `KN-01M4GT63VY4Y5K3B0554764K6V`）。
 *
 * * **记录基址**由语料夹出来，不靠约定：以 `cur` 索引的字段形态 `[reg+reg*8+5D8xxxh]`
 *   （`reg = 15·cur` ⇒ 净 `120·cur`）命中的**绝对地址最小 = `0x5D894`**、**最大 = `0x5D908`**（= 基址 `+0x74`）；
 *   区间 `0x5D870..0x5D893` 与 `0x5D90C` 以上**零处**。
 * * **`0x5D880` 一族不是记录字段**：`0x5D880/0x5D884/0x5D888/0x5D88C/0x5D890` 全语料**一个 `*8` 形态都没有**
 *   （只有绝对形态）⇒ 它们不随 `cur` 走 = **Engine 级标量**（`0x5D880` = `cur` 自身）。
 *   装载器 `sub_40ED40` 的**两种以 cur 索引**的写法因此分工明确：
 *   `Engine[0x5D880]`（`cur`）→ 算 `120·cur` → 加到**记录基址** `0x5D894` 上（见 `.text:0040EDD0` 一族，
 *   `.text:0040EDEB mov [esi+ecx*8+5D8D0h],ebx` = 记录 `+0x3C`）；而 `0x5D880 + 0x78*cur` **不是任何记录地址**
 *   （它落在记录**前** 0x14 字节的那族标量里 —— 这正是旧口径的错）。
 * * ⇒ `0x5D904/0x5D908` 相对**真基址**是 `+0x70/+0x74` ⇒ 记录大小 = `0x74 + 4 = 0x78`，
 *   与下一条记录的 `+0x00` 严丝合缝、不重叠；而相对旧基址 `0x5D880` 它们是 `+0x84/+0x88` ⇒
 *   旧读数「记录下界 ≥ `0x8C`」是**假偏移**（那条用例已改写成**负例**，见下）。
 * * 三格 `0x5D8F8/0x5D8FC/0x5D900` = 记录内 `+0x64/+0x68/+0x6C`（**帧内三格**，不是独立数组）——
 *   旧记法里它们是 `帧+0x78/0x7C/0x80`，而 `+0x78` 正好等于 stride ⇒ 才被读成"下一帧的 `+0x00`"。
 * * **旁证**（不单独当判据）：`0x5D894 + 40*0x78 = 0x5EB54` 正好是帧区之后那个成员
 *   （全语料 94 处引用）；而旧算术的 `0x5D880 + 40*0x78 = 0x5EB40` 在 `.text` 里**零处**引用。
 *
 * ★ 仍未查明（另单 / `REQ-01M48EVKSNX139B8TX0CYXZ0A1` 一族跟踪）：三格装的是什么表
 *   （8 字节元素、初值 `0xFF` 哨兵）以及由谁读值。
 *
 * 运行：`pnpm test:assets`（要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';

import { REPO_ROOT } from '../lib/paths.mjs';
// ★ 布局常量来自**知识层**（带 EA 出处，随这份镜像而变）⇒ 本守卫改模型常量也会红（变异清单里有两条钉它）
import { FRAME_LAYOUT as FRAME, FRAME_ENGINE_SCALARS } from '@amayui/age-format/src/engine/layout.mts';

/** ★ **旧口径**（基址 `0x5D880`）—— 留作分歧对照，⛔ 不许再拿它当基址 */
const OLD_BASE = 0x5d880;
/** 旧口径把那三格读成"帧+0x78/0x7C/0x80 的独立数组"（已裁决为错读，数值保留） */
const OLD_CLAIM = [0x78, 0x7c, 0x80];
const THREE_ARRAYS = [0x5d8f8, 0x5d8fc, 0x5d900];
/** 三个 per-cur dword 格的**分配点**（`sub_40ED40` 的 3 轮循环）与它的 `memset` 落点 */
const ALLOC_SITE = 0x40f5c1; // `mov [esi+ecx*4+5D8F8h],eax` —— `ecx = 30*cur`（dword 计数）⇒ 净字节步长 0x78
const MEMSET_SITE = 0x40f5ec; // `mov eax,[esi+eax*4+5D8F8h]` —— 读出同一格交给 `_memset`
/** ★ 对照物：**确认的记录字段** `记录+0x20`（装载器以 `[esi+edx*8+5D8B4h]` 写它，`edx = 15·cur`） */
const KNOWN_FRAME_SLOT_SITE = 0x40f2e9; // `mov [esi+edx*8+5D8B4h],eax`
/** ★ 反例物：**另一个 per-cur 状态块**的大小证据（清理器 `sub_40EA00` 的 `imul eax,84h` + `push 84h`，落点 `Engine+0x69334`） */
const STATE_BLOCK_SITE = 0x40eb3d; // `mov eax,ebx` → `imul eax,84h` → `lea ecx,[eax+edi+69334h]` → `push 84h ; Size`
/** 帧区之后那个成员（`0x5D894 + 40*0x78`）；旧算术给出的是 `0x5EB40`（`.text` 里零处引用） */
const AFTER_FRAME_AREA = 0x5eb54;
const OLD_ARITHMETIC_END = 0x5eb40;

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';

/** 语料里"记录相对"寻址的**任意**形态：`[reg+5D8xxh]`（扁平）/ `[reg+reg*4+5D8xxh]` / `[reg+reg*8+5D8xxh]`
 *  ★ 只认 `*8` 会**漏掉** `*4` 那三格（`reg = 30·cur`）与折叠形 —— 本仓踩过两次"窄正则 ⇒ 假的全称否定" */
const RE_ANY = /\[[^\]]*?\+(5D[89][0-9A-F]{2})h\]/gi;
/** 记录区窗口的上界（相对记录基址）—— 取够宽，好让"漏进窗口的野槽"能被反向检查抓到 */
const WINDOW = 0x98;

/**
 * 扫全语料，统计 `[`0x5D880`, `0x5D894 + 0x98`)` 里每个绝对地址的读/写次数。
 * ★ 窗口下界取**旧基址** `0x5D880`：这样"记录之前那族标量"也进表 ⇒ 可以断言**它们恰好是那 5 个**，
 *   而不是靠"我没去扫"来假装它们不存在。
 */
async function frameSlotCensus() {
  const slots = new Map();
  const rl = readline.createInterface({ input: fs.createReadStream(listing, { encoding: 'utf8' }), crlfDelay: Infinity });
  let line = 0;
  for await (const raw of rl) {
    line += 1;
    for (const m of raw.matchAll(RE_ANY)) {
      const abs = Number.parseInt(m[1], 16);
      if (abs < OLD_BASE || abs > FRAME.base + WINDOW) continue;
      const isWrite = /^[^;]*mov\s+\[[^\]]+\]\s*,/.test(raw);
      const s = slots.get(abs) ?? { abs, off: abs - FRAME.base, read: 0, write: 0, first: line };
      s[isWrite ? 'write' : 'read'] += 1;
      slots.set(abs, s);
    }
  }
  rl.close();
  return { slots: [...slots.values()].sort((a, b) => a.abs - b.abs), lines: line };
}

test('★ 记录内偏移的机械普查：`≥ stride` 的槽 **0 个**（记录字段全在 `0x78` 之内），而负偏移恰好是 `0x5D880` 那五个 Engine 级标量', { skip }, async () => {
  const { slots, lines } = await frameSlotCensus();
  assert.ok(lines > 100000, `应扫过十万行以上，实际 ${lines}`);
  assert.ok(slots.length > 20, `记录内槽位数应当有几十个，实际 ${slots.length}`);

  // ① 记录**之前**（旧基址那 0x14 字节）：恰好是那五个 Engine 级标量 —— 一个不多、一个不少
  const scalars = Object.values(FRAME_ENGINE_SCALARS);
  const before = slots.filter((s) => s.abs < FRAME.base);
  assert.deepEqual(
    before.map((s) => `0x${s.abs.toString(16)}`),
    [...scalars].sort((a, b) => a - b).map((a) => `0x${a.toString(16)}`),
    '★ 记录基址之前只许有 `FRAME_ENGINE_SCALARS` 那五个绝对地址：多一个 ⇒ 记录基址取早了；少一个 ⇒ 记录基址取晚了',
  );
  assert.equal(FRAME.base - OLD_BASE, 0x14, '记录基址比旧口径的 `0x5D880` 高 `0x14`（= 那五个标量占的 5 个 dword）');

  // ② 记录之内：全部落在 `0x00..0x74`，**一个 `≥ stride` 的都没有**（旧口径下那一个是 `+0x78`，正是误读来源）
  const inside = slots.filter((s) => s.abs >= FRAME.base);
  const beyond = inside.filter((s) => s.off >= FRAME.stride);
  assert.deepEqual(
    beyond.map((s) => `+0x${s.off.toString(16)}`),
    [],
    '★ 记录内**不许**出现 `≥ stride` 的槽：出现 ⇒ 要么 stride 不是 0x78，要么记录基址取错了（这条守卫存在的理由）',
  );
  assert.equal(Math.max(...inside.map((s) => s.off)), 0x74, '记录内**最大**偏移必须是 `+0x74`（⇒ 记录大小 = `0x74 + 4 = 0x78`）');
  assert.equal(0x74 + 4, FRAME.stride, '最大字段偏移 + 4 必须**等于**索引步长（⇒ 大小 = 步长，两个量是同一个）');

  // ③ 记录里确实有字段（不是空扫）：关键槽必须在场（用**模型常量**点名，改坏常量这条一起红）
  const offs = new Set(inside.map((s) => s.off));
  const need = [
    FRAME.off.strBase, FRAME.off.operands, FRAME.off.count0, FRAME.off.base0, FRAME.off.caller,
    FRAME.off.frameArg, ...FRAME.off.triples.flatMap((t) => [t.len, t.ptr]),
    FRAME.off.position58, FRAME.off.position5c, FRAME.off.operandCount, FRAME.off.arrayContainer,
    ...FRAME.off.grids,
  ];
  for (const off of need) assert.ok(offs.has(off), `记录内应出现 +0x${off.toString(16)}（普查漏了？）`);
  assert.deepEqual(
    [...FRAME.off.grids],
    THREE_ARRAYS.map((a) => a - FRAME.base),
    '★ 三格必须正好是记录内 `+0x64/+0x68/+0x6C`（旧记法的 `+0x78/+0x7C/+0x80`）',
  );
});

test('★ 三个 per-cur dword 格 `0x5D8F8/0x5D8FC/0x5D900`：索引是 `30·cur` 个 dword（净字节步长 = **0x78**）⇒ 记录内三格', { skip }, () => {
  const text = fs.readFileSync(listing, 'utf8');
  // ★ IDA 的写法是**去掉前导零的大写十六进制 + `h`**（`5D8F8h`）—— 用 `toString(16)` 会把 `0x5d8f8`
  //   当十进制数打成 `5D8F8H` 那种错形态（本用例第一版就是这么假红的）。两种形态都接受：
  const hexOf = (abs) => [`${abs.toString(16).toUpperCase()}h`, `${abs.toString(16).padStart(8, '0').toUpperCase()}h`];
  const form = (abs, scale) => hexOf(abs).some((h) => new RegExp(`\\[\\w{2,3}\\+\\w{2,3}\\*${scale}\\+${h}\\]`, 'i').test(text));
  for (const abs of THREE_ARRAYS) {
    assert.ok(form(abs, 4), `0x${abs.toString(16)} 应存在"按索引 ×4"的寻址形态（形如 [ecx+edx*4+5D8F8h]），语料里找不到`);
  }
  // ★ 对照物：**确认的记录字段** `5D8B4h`（记录+0x20）以 `[esi+edx*8+5D8B4h]`（`edx = 15·cur`）写。
  //   ⇒ 这一族 `*8` 形态写的是**记录内偏移**；而这三格用 `*4` + `edx = 30·cur` 写的是**同一批 cur 的另一套编号**
  //   （净字节步长**同为 0x78** ⇒ 它们与记录字段同口径，只是 codegen 写法不同）。
  const knownSlot = new RegExp(`\\[\\w{2,3}\\+\\w{2,3}\\*8\\+5D8B4h\\]`, 'i');
  assert.ok(knownSlot.test(text), '对照物 `[reg+reg*8+5D8B4h]`（确认的记录字段 +0x20）必须在语料里');
  const sameIndex = new RegExp(`mov\\s+\\[esi\\+ecx\\*4\\+5D8F8h\\],\\s*eax`, 'i');
  assert.ok(sameIndex.test(text), '`5D8F8h` 必须以 `[esi+ecx*4+…]`（`ecx = 30·cur` 个 dword）形态出现');
  // 这三个地址**落在记录区之内**（记录基址 0x5D894 起、40 份、每份 0x78 ⇒ 末地址 0x5EB54）。
  const frameEnd = FRAME.base + FRAME.stride * FRAME.count;
  for (const abs of THREE_ARRAYS) {
    assert.ok(abs > FRAME.base && abs < frameEnd, `0x${abs.toString(16)} 应落在记录区内（0x${FRAME.base.toString(16)}..0x${frameEnd.toString(16)}）`);
  }
  assert.deepEqual(
    THREE_ARRAYS.map((a) => a - FRAME.base),
    [0x64, 0x68, 0x6c],
    '★ 这三格相对**真基址** `0x5D894` 是 `+0x64/+0x68/+0x6C`（记录内的三格，不是"下一帧的 +0x00"）',
  );
  // ★ 旧读数保留成**分歧对照**：相对旧基址 0x5D880 它们正好是 `+0x78/+0x7C/+0x80`，
  //   而 `+0x78` **等于 stride** ⇒ 才被读成"下一帧的 +0x00"（错在这里，不在数值上）。
  assert.deepEqual(
    THREE_ARRAYS.map((a) => a - OLD_BASE),
    OLD_CLAIM,
    '★ 旧口径那三个"帧内偏移"要保留在判据里（`0x5D8F8` 在字节上等于"旧基址+0x78"）',
  );
  assert.equal(OLD_CLAIM[0], FRAME.stride, '★ 旧读数的 `+0x78` 正好等于 stride ⇒ 这正是"它是下一帧开头"那条误读的来源');
  // ★ 反例（把这次的教训写成断言）：语料里**没有** `[reg+reg*8+5D8F8h]` 形态
  //   ⇒ 任何"它也以记录内索引（×8）形态出现"的说法都是**没核过字节**。谁要改这条，先贴 EA。
  assert.ok(!form(0x5d8f8, 8), '★ 语料里 `5D8F8h` **没有** `*8` 形态（共 4 处：两条 `*4` + 一条 `lea` + 一条枚举）—— 别声称它有');
});

/**
 * ★★ **负例（旧读数已否，保留为分歧对照）**：`REQ-01M4GSW2R999MBJQ9S9EEQ481Q` 时代的
 * 「帧记录大小 **至少 0x8C**」是**把记录基址取成 `0x5D880`** 算出来的**假偏移**。
 *
 * * 字节事实**不变**：`0x40F63B` 写 `[esi+edx*8+5D904h]`、`0x40F678` 写 `[esi+edx*8+5D908h]`（各一次，
 *   `ecx` = `operator new(0x10)` 出来的容器对象）。
 * * 变的只是**相对谁**：`0x5D904 − 0x5D880 = +0x84`、`0x5D908 − 0x5D880 = +0x88` ⇒ 旧读数推出"下界 `0x8C`"；
 *   而 `0x5D904 − 0x5D894 = +0x70`、`0x5D908 − 0x5D894 = +0x74` ⇒ 真读数**落在 `0x78` 之内**，不越界、不重叠。
 * * 旧读数为什么错：它把 `0x5D880`（= `cur` 这个 **Engine 级标量**的槽）当成了 per-cur 记录的基址；
 *   而 `0x5D880` 族**零个 `*8` 形态**（见下一条用例）⇒ 它根本不是数组基址。
 * * ⛔ 另一条老坑也留着：`imul eax,84h`（`0x40EB3F`）**不是**记录大小 ——
 *   它的落点是 `Engine+0x69334 + 0x84*cur`（**另一个** per-cur 状态块，离记录区很远）。
 */
test('★★ 负例：旧口径「记录大小 ≥ 0x8C」= 把基址取成 `0x5D880` 的假偏移（真读数 `+0x70/+0x74` 落在 `0x78` 之内）', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const at = (ea) => lines.findIndex((l) => l.toLowerCase().includes(`.text:${ea.toString(16).padStart(8, '0')} `));
  // ① 字节事实（两种读法共用）：两条写必须在场
  const i1 = at(0x40f63b), i2 = at(0x40f678);
  assert.ok(i1 > 0 && i2 > i1, '应能找到 `[esi+edx*8+5D904h]`（0x40F63B）与 `[esi+edx*8+5D908h]`（0x40F678）两条写');
  assert.match(lines[i1], /\[esi\+edx\*8\+5D904h\]/i, `0x40F63B 应是写 \`5D904h\`；实际：${lines[i1]}`);
  assert.match(lines[i2], /\[esi\+edx\*8\+5D908h\]/i, `0x40F678 应是写 \`5D908h\`；实际：${lines[i2]}`);
  // ② 旧读法（假偏移）：相对 0x5D880 是 +0x84/+0x88 ⇒ 推出"下界 0x8C > stride 0x78"
  assert.equal(0x5d904 - OLD_BASE, 0x84, '旧基址下 `0x5D904` 记作 `帧+0x84`（**假偏移**）');
  assert.equal(0x5d908 - OLD_BASE, 0x88, '旧基址下 `0x5D908` 记作 `帧+0x88`（**假偏移**）');
  assert.ok(0x88 + 4 > FRAME.stride, '旧读数由此得出 `0x8C > 0x78` ⇒ "两个不同的量"—— 这条**算术**没错，错在**基址**');
  // ③ 真读数（相对记录基址）：落在记录之内 ⇒ 大小 = 步长，没有第二个量
  assert.equal(0x5d904 - FRAME.base, FRAME.off.arrayContainer, '真读数：`0x5D904` = 记录 + `arrayContainer`');
  assert.equal(0x5d908 - FRAME.base, 0x74, '真读数：`0x5D908` = 记录 + `0x74` = 最大字段偏移');
  assert.ok(0x74 + 4 === FRAME.stride, '★ 真读数之下"最大偏移 + 4"**等于** stride ⇒ 记录大小与索引步长是同一个量');
  // ④ ⛔ 老坑：`imul eax,84h` 不是记录大小 —— 它是另一个 per-cur 状态块（落点 Engine+0x69334）
  const i3 = at(STATE_BLOCK_SITE);
  assert.ok(i3 > 0, `应能找到 \`0x${STATE_BLOCK_SITE.toString(16)}\`（\`imul eax,84h\` 那一行）`);
  const win = lines.slice(i3, i3 + 8).join('\n');
  assert.match(win, /imul\s+eax,\s*84h/i, `那个 0x84 块必须是 \`imul eax,84h\` 算出来的；实际窗口：\n${win}`);
  assert.match(win, /lea\s+ecx,\s*\[eax\+edi\+69334h\]/i, `它的落点必须是 \`Engine+0x69334 + 0x84*cur\`（**不是**记录区）—— 实际窗口：\n${win}`);
});

/**
 * ★★ 记录基址/大小的判据：**只认以 cur 索引的形态**，两端都真实出现，区间外零处。
 * ★ 算式全部走**模型常量**（`FRAME.base` / `FRAME.stride`）⇒ 把常量改回 `0x5D880`（或把 `0x78` 改成 `0x8C`）
 *   这条**当场红**（变异清单里那两条钉的就是它）。
 */
test('★★ 帧记录 = `0x5D894` 起 `0x78` 字节：以 cur 索引的字段绝对地址全在 `0x5D894..0x5D908`，而 `0x5D880` 一族零个 `*8` 形态（⇒ 记录大小 = 索引步长 = `0x78`）', { skip }, () => {
  const text = fs.readFileSync(listing, 'utf8');
  const abs = new Set();
  for (const m of text.matchAll(/\[[a-z0-9]+\+[a-z0-9]+\*8\+(5D[89][0-9A-F]{2})h\]/gi)) abs.add(Number.parseInt(m[1], 16));
  const nums = [...abs].sort((a, b) => a - b);
  assert.ok(nums.length > 10, `以 cur 索引的字段形态应当命中十几个绝对地址，实际 ${nums.length}`);
  assert.equal(nums[0], FRAME.base, `以 cur 索引的字段**最小**绝对地址必须是记录基址（模型 ${`0x${FRAME.base.toString(16)}`}）；实际 0x${nums[0].toString(16)}`);
  assert.equal(nums[nums.length - 1], FRAME.base + 0x74, `**最大**绝对地址必须是 基址 +0x74（= 0x5D908）；实际 0x${nums[nums.length - 1].toString(16)}`);
  const out = nums.filter((n) => n < FRAME.base || n > FRAME.base + 0x74);
  assert.deepEqual(out.map((n) => `0x${n.toString(16)}`), [], '★ 记录字段不许落在 `[基址, 基址+0x74]` 之外 —— 落出去就说明"记录基址/大小"这两个量又混了');
  // ★ 但**不是** 30 个都写成 `*8`：另外四格用**别的形态**（这正是"只认字面 `*8` 会假红"的教训）
  const OTHER_FORMS = new Map([
    [0x24, '折叠形（`add ecx,0C79h`，`0xC79 × 0x78 = 0x5D8B8`）；字面 `5D8B8h` 零次'],
    [0x58, '扁平形（基址提升进寄存器：`mov [eax+5D8ECh],edx`）'],
    [0x64, '`*4` 形态（`reg = 30·cur`，净步长同为 `0x78`）'],
    [0x68, '`*4` 形态'],
    [0x6c, '`*4` 形态'],
  ]);
  const offs = nums.map((n) => n - FRAME.base);
  const all = Array.from({ length: 30 }, (_, i) => 4 * i);
  const absent = all.filter((o) => !offs.includes(o));
  assert.deepEqual(
    absent.map((o) => `+0x${o.toString(16)}`),
    [...OTHER_FORMS.keys()].map((o) => `+0x${o.toString(16)}`),
    '★ 只有这几格**不**以 `*8` 字面形态出现（多一格/少一格都说明口径混了）：\n  ' +
      [...OTHER_FORMS].map(([o, why]) => `+0x${o.toString(16)}：${why}`).join('\n  '),
  );
  assert.equal(offs.length, 30 - OTHER_FORMS.size, '★ 30 格记录里除上面那几格外，其余都以 `*8` 形态出现');
  // ② Engine 级标量：那五个**只有绝对形态**（一个 `*8` 都没有）
  for (const scalar of Object.values(FRAME_ENGINE_SCALARS)) {
    const h = scalar.toString(16);
    assert.ok(
      !new RegExp(`\\*8\\+${h}h\\]`, 'i').test(text),
      `★ 0x${h.toUpperCase()} 不该有 \`*8\`（per-cur）形态：有它 ⇒ 记录基址就得往前挪，本条判据当场红`,
    );
  }
  // ③ 记录大小 = 步长（算术自证）：最大字段偏移 `+0x74`，`+4` 正好是下一条记录
  assert.equal(0x74 + 4, FRAME.stride, '★ 记录内最大偏移 + 4 必须**等于**索引步长 0x78（⇒ 大小 = 步长，两个量是同一个）');
  // ④ 边界旁证：`基址 + 40*0x78` 那个成员真实存在；旧算术的地址在 `.text` 里零处引用
  assert.equal(FRAME.base + FRAME.stride * FRAME.count, AFTER_FRAME_AREA, '★ 40 份记录的末尾 = `0x5EB54`（帧区之后的那个成员）');
  assert.ok(text.includes('5EB54h'), '`0x5EB54` 必须在语料里出现（全语料 94 处引用 —— 它是"40 帧"的边界判据）');
  assert.ok(!text.includes('5EB40h'), '★ 旧算术的 `0x5EB40`（= `0x5D880 + 40*0x78`）在 `.text` 里**零处**引用 —— 旧基址下这个边界讲不通');
  // ⑤ 字节事实（上一条负例也钉着，这里再钉一次：结论变的是"相对基址"，不是这两条写）
  assert.match(text, /\[esi\+edx\*8\+5D904h\]/i, '`0x40F63B` 那条写必须在场');
  assert.match(text, /\[esi\+edx\*8\+5D908h\]/i, '`0x40F678` 那条写必须在场');
});

test('★★ 裁决依据（`sub_40ED40` 的分配点）：`0x5D8F8` 那格是 `operator new[]` 的落点，紧跟 `_memset(ptr,0FFh,8+8n)`', { skip }, () => {
  const text = fs.readFileSync(listing, 'utf8');
  const lines = text.split('\n');
  // ★ 语料里的 EA 是**大写零填充**（`.text:0040F5C1`）⇒ 归一后再比（本用例第一版拿小写去比，假红）
  const at = (ea) => lines.findIndex((l) => l.toLowerCase().includes(`.text:${ea.toString(16).padStart(8, '0')} `));
  const iAlloc = at(ALLOC_SITE);
  const iMemset = at(MEMSET_SITE);
  assert.ok(iAlloc > 0, `应能找到 \`0x${ALLOC_SITE.toString(16)}\`（写 \`[esi+ecx*4+5D8F8h]\` 那一行）`);
  assert.ok(iMemset > iAlloc, `应能找到它后面的 \`0x${MEMSET_SITE.toString(16)}\`（读出同一格交给 memset）`);
  // ★★ 判据三件套（都不靠"某个偏移出现过"，而靠**这几条指令的次序**）：
  //   ① 写这一格的索引必须由 `15·cur` 折成 `30·cur`（⇒ 净字节步长 = 0x78 = 记录步长）；
  //   ② 它必须是 `operator new[]` 的落点（⇒ 这一格装的是**指针**）；
  //   ③ 同一个循环里 `_memset(ptr,0FFh,8+8n)`。
  assert.match(lines[iAlloc], /\[esi\+ecx\*4\+5D8F8h\]/i, `0x${ALLOC_SITE.toString(16)} 应是 \`mov [esi+ecx*4+5D8F8h],eax\`（ecx = 30·cur，dword 计数）；实际：${lines[iAlloc]}`);
  const before = lines.slice(Math.max(0, iAlloc - 8), iAlloc).join('\n');
  assert.match(before, /shl\s+edx,\s*4/i, `写 0x5D8F8 之前应有一条 \`shl edx,4\`（15·cur 的一半）；实际窗口：\n${before}`);
  assert.match(before, /sub\s+edx,\s*ecx/i, `写 0x5D8F8 之前应有一条 \`sub edx,ecx\`（⇒ edx = 15·cur）；实际窗口：\n${before}`);
  assert.match(before, /lea\s+ecx,\s*\[edi\+edx\*2\]/i, `写 0x5D8F8 之前应有一条 \`lea ecx,[edi+edx*2]\`（⇒ ecx = 30·cur = **记录步长的 dword 数**）；实际窗口：\n${before}`);
  assert.match(before, /call\s+\?\?_U@YAPAXI@Z/i, `同一个循环里应先有 \`operator new[](uint)\`（这一格是**指针**）；实际窗口：\n${before}`);
  const win = lines.slice(iMemset, iMemset + 6).join('\n');
  assert.match(win, /call\s+_memset/i, `\`0x${MEMSET_SITE.toString(16)}\` 之后应出现 \`call _memset\`；实际：\n${win}`);
  const pre = lines.slice(Math.max(0, iMemset - 6), iMemset + 1).join('\n');
  assert.match(pre, /push\s+0FFh/i, `memset 之前应有一条 \`push 0FFh\`（Val —— 三张表初值全 0xFF 的判据）；实际窗口：\n${pre}`);
  assert.match(pre, /lea\s+edx,\s*ds:8\[ecx\*8\]/i, `memset 的 Size = \`8 + 8*n\`（元素 8 字节）；实际窗口：\n${pre}`);
  assert.match(pre, /mov\s+eax,\s*\[esi\+eax\*4\+5D8F8h\]/i, 'memset 的指针必须**回读同一格**（证明这一格装的是指针，不是整数）；实际窗口：\n' + pre);
  // ★ 对照物：确认的记录字段 `+0x20` 的分配步序（`[esi+edx*8+5D8B4h]`，`edx = 15·cur`）——
  //   它证明这一族 `*8` 形态写的就是**记录内偏移**，而这三格的 `*4` 形态是**同一批 cur 的另一套编号**。
  const iKnown = at(KNOWN_FRAME_SLOT_SITE);
  assert.ok(iKnown > 0, `应能找到对照物 \`0x${KNOWN_FRAME_SLOT_SITE.toString(16)}\`（写 \`[esi+edx*8+5D8B4h]\`）`);
  assert.match(lines[iKnown], /\[esi\+edx\*8\+5D8B4h\]/i, `对照物应是 \`mov [esi+edx*8+5D8B4h],eax\`（记录+0x20）；实际：${lines[iKnown]}`);
});

test('★ 记录的那三个数（base/stride/count）**已裁决**：改任何一个都要先给出裁决依据（EA / 语料 / 守卫）', () => {
  // ★ 本用例的职责 = 把**分歧的数值**钉住 + 把**已裁决的归属**写死。
  //   旧口径把 `0x5D880` 当记录基址、把那三个绝对地址读成"帧+0x78/0x7C/0x80 的独立数组"
  //   ⇒ 已裁决为**错读**（见文件头），但那一侧的数值**保留**在这里当对照物：谁要改它们，必须先说明凭什么改。
  assert.deepEqual(OLD_CLAIM, [0x78, 0x7c, 0x80], '旧口径那三个"记录内偏移"是分歧的一方（已判错读），不许静默改写');
  assert.equal(THREE_ARRAYS[0] - OLD_BASE, OLD_CLAIM[0], '★ 这正是当年误读的来源：`0x5D8F8` 在字节上等于"旧基址 + 0x78"');
  assert.equal(FRAME.base - OLD_BASE, 0x14, '新旧基址相差 `0x14`（= `0x5D880` 那五个 Engine 级标量）');
  assert.equal(FRAME.base + FRAME.stride * FRAME.count, AFTER_FRAME_AREA, '40 份记录的末尾 = `0x5EB54`（帧区之后的那个成员）');
  assert.ok(FRAME.stride * FRAME.count > OLD_CLAIM[2], '步长与帧数决定的记录区**大于**旧口径点名的那些偏移');
});

test('★ 记录的 base/stride/count 本身就是"已裁决的口径"：改任何一个都要先裁决', () => {
  assert.equal(FRAME.base, 0x5d894, '记录基址 `0x5D894`（★ 历史错：曾把 `0x5D880` 当基址 —— 那是 `cur` 的槽）');
  assert.equal(FRAME.stride, 0x78, '步长 `0x78` —— 三处独立机械证据（两条取操作数原语 + 字段清零循环）');
  assert.equal(FRAME.count, 40, '槽数 40（`call-script` 的 `cur >= 39` 抛「階層が深すぎます」）');
  assert.equal(FRAME.base + FRAME.stride * FRAME.count, 0x5eb54, '40 份记录的末尾 = `0x5EB54`（语料 94 处引用；旧算术的 `0x5EB40` 为零）');
});
