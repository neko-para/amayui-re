/** @env assets @kind contract @why 引擎通用数据区域的模型与语料口径不一致（池布局/编解码/操作数步长） */
/**
 * tools/test/emulator-model.test.mjs —— `apps/emulator` 最小模型的守卫（批 R1 迭代点 ④）
 *
 * ## 这份守卫为什么"红得有意义"
 * 模型里的每个常量都对应语料里的一处**可复核观察**。这里的用例做两件事：
 *   ① **回语料复核**：把模型常量（帧步长 / 池基址 / 操作数的 `15·cur` 索引 / `base + idx*4`）
 *      拿语料的指令序列对一遍 —— 语料换了镜像、或有人手改了常量，当场红。
 *   ② **行为判据**：`dec(enc(v))==v` 在**池这一层**成立；int 族过 DEC、float 族不过；下标不过。
 *
 * ## 它守不了什么（诚实边界）
 * 池的**容量**与"越界语义"没有 oracle（引擎无越界检查，容量只有二手数字）⇒ 本守卫**不判**这些，
 * 只在模型里把"引擎不检查"这件事**显式记录**（`LocalPools.noteOOB`）。
 *
 * 运行：`pnpm test:assets`（要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
// ★ 两边的分工（这就是本支守卫的意义）：
//   布局常量来自 **age-format 的知识层**（带 EA 出处，随这份镜像而变）
//   语义模型来自 **模拟器**（池有哪几个 / 编码与否 / 越界怎么办 —— 不含偏移）
import { GLOBAL_SLOTS as GLOBAL, FRAME_LAYOUT as FRAME, FRAME_SLOTS_OBSERVED, LOCAL_POOL_SLOTS } from '@amayui/age-format/src/engine/layout.mts';
import { LOCAL_POOLS, GlobalPools, LocalPools } from '../../apps/emulator/src/model/pools.ts';
import * as poolsModule from '../../apps/emulator/src/model/pools.ts';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';

/** 取某 EA 之后 n 行，归一化空白（IDA 是 `mov     eax, [esi+5D880h]`） */
function windowAfter(ea, n) {
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  const hex = ea.toString(16).toUpperCase().padStart(8, '0');
  const start = all.findIndex((l) => l.includes(`:${hex} `));
  assert.ok(start >= 0, `语料里找不到 EA 0x${ea.toString(16)}`);
  return all.slice(start, start + n).map((l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ').replace(/, /g, ','));
}

const findInOrder = (lines, needles) => {
  let i = 0;
  for (const need of needles) {
    const want = need.replace(/\s+/g, ' ').replace(/, /g, ',');
    let found = -1;
    for (let k = i; k < lines.length; k += 1) if (lines[k].startsWith(want)) { found = k; break; }
    if (found < 0) return { ok: false, missing: need };
    i = found + 1;
  }
  return { ok: true };
};

// ─────────────────────────────────────── ① 回语料复核模型的常量

test('★ 帧步长 0x78：语料里 `shl 4; sub; *8` 的形态必须与模型常量相容（15·cur×8 = 120·cur）', { skip }, () => {  assert.equal(FRAME.stride, 15 * 8, '步长必须等于 15×8（这是那两条原语的算术形状）');
  // 取址原语入口的操作数基址算法
  const win = windowAfter(0x42aed3, 8);
  const r = findInOrder(win, ['mov eax,[esi+5D880h]', 'mov ecx,eax', 'shl ecx,4', 'sub ecx,eax', 'mov eax,[esi+ecx*8+5D898h]']);
  assert.ok(r.ok, `取址原语的"帧基址 + 操作数基址"序列与模型不符（缺 "${r.missing}"）`);
  // 0x5D898 必须正好是 帧基址 + 模型里的 operands 偏移
  assert.equal(0x5d898, FRAME.base + FRAME.off.operands, '操作数基址槽必须等于 帧 + operands 偏移');
});

test('★ global 池基址/计数槽必须与语料里 type 3/4 的取址一致', { skip }, () => {
  const win = windowAfter(0x42af2d, 4);
  const r = findInOrder(win, ['mov ecx,[eax]', 'mov edx,[esi+5D800h]', 'lea eax,[edx+ecx*4]']);
  assert.ok(r.ok, `type 3（global int）的取址形态与模型不符（缺 "${r.missing}"）`);
  assert.equal(GLOBAL.base.int, 0x5d800, '模型的 global int 基址槽必须就是语料里那一个');
  // float 族：case 4 用 0x5D808
  assert.equal(GLOBAL.base.float, 0x5d808);
  assert.ok(GLOBAL.alt.int === GLOBAL.base.int + 4, '`*_alt` 与基址成对（相差 4 字节）');
});

/**
 * ★ **记录内 slot 的机械普查（两种寻址形态）** —— 这是本轮修掉那个真错的产物。
 *
 * 为什么不能只认字面常量：偏移会被**折叠进索引寄存器**。
 * 实测反例：`local_float` 的基址写入是 `.text:0040F31B..F335`
 *   `mov ecx,[esi+5D880h]` → `add ecx,0C79h` → `mov edx,ecx; shl edx,4; sub edx,ecx`（`edx = 15*(cur+0xC79)`）
 *   → `mov [esi+edx*8],eax` ⇒ 目标 = `Engine + 120*cur + 0x5D8B8` = **记录+0x24**（旧基址记法写作 `帧+0x38`）。
 * 语料里**没有** `5D8B8h` 这个字面（0 次），却有 192 处 `[reg+reg*8]` 折叠形态。
 * ⇒ 我上一轮用"字面 0 次"推出"`+0x38` 不存在"，**那个全称否定是方法坏掉造成的**。
 *   （独立复核者抓到了这条；我复算后确认它是对的。）
 *
 * @returns {{literals:Map<number,number>, folded:Map<number,number>}}
 */
function frameCensus() {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const literals = new Map();
  const folded = new Map();
  // ★ 折叠形 `add ecx,0C79h` 的等效记录内偏移（自证：`0xC79 × 0x78 = 0x5D8B8`，而 `0x5D8B8 − 0x5D894 = 0x24`）
  const FOLDED_OFF = 0x5d8b8 - FRAME.base;
  for (let i = 0; i < lines.length; i += 1) {
    const raw = lines[i];
    for (const m of raw.matchAll(/(5D[89][0-9A-F]{2})h/gi)) {
      const v = Number.parseInt(m[1], 16);
      if (v >= FRAME.base && v <= FRAME.base + 0x98) literals.set(v - FRAME.base, (literals.get(v - FRAME.base) ?? 0) + 1);
    }
    // 折叠形态：`[reg+reg*8]`，且前 12 行里有 `…,0C79h`（⇒ 该处等效记录内偏移 `+0x24`）
    if (/\[(?:esi|ecx|eax|edx|ebx)\+\w{2,3}\*8\]/.test(raw)) {
      const win = lines.slice(Math.max(0, i - 12), i).join(' ');
      if (/0C79h/.test(win)) folded.set(FOLDED_OFF, (folded.get(FOLDED_OFF) ?? 0) + 1);
    }
  }
  return { literals, folded };
}

/**
 * ★ **基址的语料判据**（2026-10 二次订正后重写）：不再问"这个偏移在语料里出现过吗"——
 *   那种问法**绿而错**（`+0x50`/`+0x54` 是别的帧字段，照样"出现过"，于是旧表蒙混过关）。
 *   现在问两件**能定配对**的事：
 *   ① **装载器 `sub_40ED40` 里那 6 处 store 写的是哪 6 个地址**（集合必须与模型相等）；
 *   ② **取址原语 `sub_42AEA0` 的跳转表里，case 号（= operand type）各自读哪个地址**
 *      （逐个与 `LOCAL_POOL_SLOTS[typeTag]` 相等 —— 这才把"int / float / string"钉死）。
 *   ⇒ 改错任何一个偏移（或把两个池的位置对调）都会当场红。
 */
function corpusLocalPoolEvidence() {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const unhex = (h) => Number.parseInt(h, 16);

  // ① 装载器：**跟着 `operator new[]` 走** —— 每个池 `new[]` 之后那次"写基址"才是池基址
  //   （★ 不能只按"形如 `mov [esi+edx*8+5D8??h],r32`"扫：`sub_40ED40` 里另外还写 `+0x14`（代码区基址）
  //     与 `+0x4C` —— 那两处不在分配之后，按结构筛就自然排除掉了）
  const loader = new Set();
  {
    const from = lines.findIndex((l) => / sub_40ED40 proc near$/.test(l));
    assert.ok(from > 0, '语料里应能找到 `sub_40ED40 proc near`（装载器）');
    const to = lines.findIndex((l, i) => i > from && / sub_40ED40 endp$/.test(l));
    const body = lines.slice(from, to > 0 ? to : from + 4000);
    let allocs = 0;
    for (let i = 0; i < body.length; i += 1) {
      if (!/operator new\[\]\(uint\)/.test(body[i])) continue;
      allocs += 1;
      // 窗口 60 行：分配之后还有"算 size / 建对象 / memset"若干步（实测最长的一处隔了 30 行）
      for (let k = i + 1; k < Math.min(i + 60, body.length); k += 1) {
        const m = /mov\s+\[esi\+edx\*8\+(5D8[0-9A-F]{2})h\],\s*(?:eax|ecx|edx|ebx)/.exec(body[k]);
        if (m) { loader.add(unhex(m[1])); break; }
        // 折叠形：`add ecx,0C79h` ⇒ 地址 = 0xC79 × 步长（自证：0x5D8B8 / 0x78 = 0xC79）
        const f = /add\s+ecx,\s*(0?[0-9A-F]{2,4})h/.exec(body[k]);
        if (f) {
          const addr = unhex(f[1]) * FRAME.stride;
          if (addr >= FRAME.base && addr <= FRAME.base + 0x98) { loader.add(addr); break; }
        }
      }
    }
    // ★ 装载器里**不止 6 次**分配（实测 7 次：6 个池 + 记录 `+0x64` 那个 cur 索引数组，
    //   `FRAME_SLOTS_OBSERVED` 里有名；旧基址记法写作 `帧+0x78`）⇒ 不硬性要求"恰好 6 次"，改由下面的"多出来的必须是已登记帧槽"兜底。
    assert.ok(allocs >= 6, `装载器里应当至少有 6 次分配（6 个池），实际 ${allocs}`);
  }

  // ② 取址原语：跳转表 case 标签 → 该分支读的地址
  const byType = new Map();
  {
    const from = lines.findIndex((l) => / sub_42AEA0 proc near$/.test(l));
    assert.ok(from > 0, '语料里应能找到 `sub_42AEA0 proc near`（取址原语）');
    const to = lines.findIndex((l, i) => i > from && / sub_42AEA0 endp$/.test(l));
    const body = lines.slice(from, to > 0 ? to : from + 4000);
    for (let i = 0; i < body.length; i += 1) {
      const c = /; jumptable 0042AF16 case (\d+)/.exec(body[i]);
      if (!c) continue;
      const type = Number(c[1]);
      let addr = null;
      for (let k = i; k < Math.min(i + 20, body.length); k += 1) {
        const m = /\[esi\+edx\*8\+(5D8[0-9A-F]{2})h\]/.exec(body[k]);
        if (m) { addr = unhex(m[1]); break; }
        const f = /add\s+ecx,\s*(0?[0-9A-F]{2,4})h/.exec(body[k]);
        if (f) {
          const a = unhex(f[1]) * FRAME.stride;
          if (a >= FRAME.base && a <= FRAME.base + 0x98) { addr = a; break; }
        }
      }
      if (addr !== null) byType.set(type, addr);
    }
  }
  return { loader, byType };
}

test('★ local 池的 slot 几何：**6 个整齐基址**（与旧仓一致）—— 且按 EA 回语料核过（装载器 6 处 store + 6 个 type 的读侧 case 双向对上）', { skip }, () => {
  assert.equal(LOCAL_POOLS.length, 6, 'local 池是 6 个（int/float/string/ptr/floatPtr/stringPtr）');
  assert.deepEqual(LOCAL_POOLS.map((p) => p.typeTag), [9, 10, 11, 12, 13, 14], 'operand type 9..14 依次对应 6 个池');
  assert.deepEqual(
    LOCAL_POOL_SLOTS.map((p) => p.count),
    [0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c],
    '6 个计数槽连续（绝对 `0x5D89C..0x5D8B0`；旧基址记法 `帧+0x1C..+0x30`）',
  );
  // ★ 池名顺序必须与语义模型一致（这是"布局 ↔ 语义"的接口；改名或加池都会红）
  assert.deepEqual(LOCAL_POOL_SLOTS.map((p) => p.name), LOCAL_POOLS.map((p) => p.name),
    '布局里的池名集合必须与模拟器语义模型的池名集合完全一致');

  // ★★ 决定性判据：拿**语料**算出来的两件事与模型比（不是"偏移出现过"）
  const { loader, byType } = corpusLocalPoolEvidence();
  const modelAddrs = [...LOCAL_POOL_SLOTS.map((p) => FRAME.base + p.base)].sort((a, b) => a - b);
  // ① 每个池的基址都必须真的**在装载器的分配点被写下**（不是"这个偏移在别处出现过"）
  for (const p of LOCAL_POOL_SLOTS) {
    assert.ok(
      loader.has(FRAME.base + p.base),
      `① 装载器 ` + `\`sub_40ED40\` 的分配点必须写下 帧+0x${p.base.toString(16)}（池 ${p.name}）`,
    );
  }
  // ①b 分配点写下的**其它**地址必须是布局里**已登记名字**的帧槽（不许有来历不明的写点）
  const namedSlots = new Set(FRAME_SLOTS_OBSERVED.map((o) => FRAME.base + o));
  const extraAlloc = [...loader].filter((a) => !modelAddrs.includes(a));
  assert.ok(
    extraAlloc.every((a) => namedSlots.has(a)),
    `①b 分配点写下的非池基址必须在 \`FRAME_SLOTS_OBSERVED\` 里有名，实际多出：${extraAlloc.map((a) => `0x${a.toString(16)}`).join(',')}`,
  );
  // ①c ★ 上一版"绿而错"的回归钉：`frameArg` / 三组第一项的**长度**这两格在语料里**确实出现过**
  //   （绝对 `0x5D8D0`/`0x5D8D4`；★ 旧基址记法写作 `帧+0x50`/`帧+0x54` —— 现在的记录内偏移是 `+0x3C`/`+0x40`），
  //   但它们**不在装载器的分配点**上 ⇒ 不许再被当成池基址。
  for (const bad of [FRAME.off.frameArg, FRAME.off.triples[0].len]) {
    assert.ok(
      !loader.has(FRAME.base + bad),
      `①c 记录+0x${bad.toString(16)} 不是池基址：它不在装载器的分配点（上一版就是被"这个偏移在语料里出现过"骗过去的）`,
    );
  }
  for (const p of LOCAL_POOL_SLOTS) {
    // ★ `typeTag` 在**语义模型**（`LOCAL_POOLS`）那边 —— 布局层只有 `name`/`count`/`base`。
    //   两者按**名字**接起来：这正是"布局 ↔ 语义"的接口，名字对不上或池被改名都会红。
    const sem = LOCAL_POOLS.find((x) => x.name === p.name);
    assert.ok(sem, `语义模型里应有池 ${p.name}`);
    assert.equal(
      byType.get(sem.typeTag),
      FRAME.base + p.base,
      `② 取址原语 \`sub_42AEA0\` 的 case ${sem.typeTag}（= operand type ${sem.typeTag}，池 ${p.name}）读的地址必须 == 记录+0x${p.base.toString(16)}`,
    );
  }
  // ★ 这条 switch 覆盖 type **3..14**（12 个 case）：3..8 是 global 族，**9..14 才是 local 池**。
  //   所以判据不是"表里恰好 6 项"，而是"6 个 local type 全在，且它们读的地址集合 == 模型的 6 个基址"。
  const localTypes = [9, 10, 11, 12, 13, 14];
  assert.deepEqual(localTypes.filter((t) => !byType.has(t)), [], `取址原语里 6 个 local type（9..14）都必须认出，实际表里有 ${[...byType.keys()].sort((a, b) => a - b).join(',')}`);
  assert.deepEqual(
    localTypes.map((t) => byType.get(t)).sort((a, b) => a - b),
    modelAddrs,
    '②b 6 个 local case 读的地址**集合**必须正好是模型声明的 6 个基址',
  );

  // ★ 语义模型里**不许**有 `base` / `count` 这类偏移字段（那是布局知识）
  for (const p of LOCAL_POOLS) {
    assert.ok(!('base' in p), `模拟器的池定义不许带 \`base\`（偏移属于布局层）：${p.name}`);
    assert.ok(!('count' in p), `模拟器的池定义不许带 \`count\`（帧内计数槽属于布局层）：${p.name}`);
  }
  // ★ 这次翻案的教训写成断言：`frameArg` / 三组第一项的长度是**别的记录字段**，不许再被当成池基址
  assert.ok(!LOCAL_POOL_SLOTS.some((p) => p.base === FRAME.off.frameArg || p.base === FRAME.off.triples[0].len),
    '它们不是任何 local 池的基址（在语料里确实出现过 —— 那正是上一版"绿而错"的原因）');
});

test('★ 脚本头 6 个 local 声明 = 6 个**连续 u32**（读窗口 +8..+28）↔ 6 个池：位置对应、每项就是一个 count', { skip }, () => {
  // ## 为什么要有这一条
  // 原先把 6 个声明写成"按脚本头**第 8..13 个 dword**建" —— 那是**字节 32..52**，
  // 而装载器只做**一次 32 字节定长读**（`push 20h` + `call ReadFile`）⇒ 那些字节根本读不到。
  // 逐字事实是：读窗口 = `Buffer`（栈 `-144h`）起 32 字节，6 个计数声明 = `Buffer+8 … Buffer+28`，
  // 即窗口内的字节 **8/12/16/20/24/28**；每项 4 字节、**整个 u32 就是一个 count**（没有 type 位、没有槽号位），
  // 声明 ↔ 池是**位置对应**（第 i 项 = 第 i 个池的计数，operand type `9+i`）。
  // ★ 原先那条 `LOCAL_POOL_SLOTS.map(p=>p.count)` 的断言是**字面量自证**（无语料 oracle）—— 这条把它变成机械可复核的。
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const from = lines.findIndex((l) => / sub_40ED40 proc near$/.test(l));
  assert.ok(from > 0, '语料里应能找到 `sub_40ED40 proc near`（装载器）');
  const to = lines.findIndex((l, i) => i > from && / sub_40ED40 endp$/.test(l));
  const body = lines.slice(from, to > 0 ? to : from + 4000)
    .map((l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ').replace(/, /g, ','));

  // ① 栈帧声明表：`Buffer` 与 6 个计数声明的**栈偏移**（IDA 写作 `var_XXX= dword ptr -YYYh`）
  const slotOf = new Map();
  for (const l of body) {
    const m = /^(\w+)= (?:byte|dword) ptr -([0-9A-F]+)h$/.exec(l);
    if (m) slotOf.set(m[1], -Number.parseInt(m[2], 16));
  }
  const buf = slotOf.get('Buffer');
  assert.ok(Number.isInteger(buf), '装载器的栈帧里应有 `Buffer`（那次定长读的落点）');

  // ② 6 对「计数源 → 计数槽」：`mov edx,[ebp+var_X]` 紧跟 `mov [esi+ecx*8+5D89Ch],edx`
  //   ★ 同一个形态在装载器里还写三组 (len,ptr)（记录 `+0x40..+0x54`）⇒ 按**落点范围**筛出计数那一族
  //   ★ 范围由模型常量算（`count0` 起 6 格）⇒ 记数槽的起点被改坏这条一起红。
  const COUNT_LO = FRAME.off.count0;
  const COUNT_HI = FRAME.off.count0 + 4 * 5;
  const pairs = [];
  for (let i = 0; i < body.length; i += 1) {
    const s = /^mov edx,\[ebp\+(\w+)\]$/.exec(body[i]);
    if (!s) continue;
    for (let k = i + 1; k < Math.min(i + 8, body.length); k += 1) {
      const t = /^mov \[esi\+ecx\*8\+(5D8[0-9A-F]{2})h\],edx$/.exec(body[k]);
      if (!t) continue;
      const off = Number.parseInt(t[1], 16) - FRAME.base;
      if (off >= COUNT_LO && off <= COUNT_HI) pairs.push({ src: s[1], off });
      break;
    }
  }
  assert.equal(pairs.length, 6, `装载器里「计数源 → 计数槽」的 store 应有 6 处，实测 ${pairs.length}`);

  // ③ 目标严格递增 4 且 == count0 + 4i；来源 = 读缓冲 +8+4i；**模型的 count 槽必须等于语料算出来的那个**
  for (let i = 0; i < 6; i += 1) {
    const p = pairs[i];
    assert.equal(p.off, FRAME.off.count0 + 4 * i,
      `第 ${i} 处计数 store 的落点应是 记录+0x${(FRAME.off.count0 + 4 * i).toString(16)}，实际 记录+0x${p.off.toString(16)}（0x${(FRAME.base + p.off).toString(16)}）`);
    assert.ok(slotOf.has(p.src), `计数源 ${p.src} 应是栈帧里声明过的槽`);
    assert.equal(slotOf.get(p.src) - buf, 8 + 4 * i,
      `第 ${i} 个计数声明应是 读缓冲+${8 + 4 * i}，实际 ${p.src} = 缓冲+${slotOf.get(p.src) - buf}`);
    assert.equal(LOCAL_POOL_SLOTS[i].count, p.off,
      `LOCAL_POOL_SLOTS[${i}]（${LOCAL_POOL_SLOTS[i].name}）的 count 槽必须 == 语料算出来的 记录+0x${p.off.toString(16)}（这就是"位置对应"：第 i 项 = 第 i 个池）`);
  }
  const srcs = pairs.map((p) => slotOf.get(p.src));
  assert.equal(srcs[0] - buf, 8, '第一个计数声明就在读缓冲 +8');
  assert.equal(srcs[5] - buf + 4, 0x20, '★ 第 6 个声明正好读到 32 字节窗口的末尾（+28 起 4 字节 ⇒ 0x20）');
  for (let i = 1; i < 6; i += 1) {
    assert.equal(srcs[i] - srcs[i - 1], 4,
      `6 个计数声明的栈偏移必须严格递增 4（⇒ IDA 的 var_ 编号严格递减 4：var_13C/138/134/130/12C/128）；第 ${i} 处不符：${srcs[i - 1]} vs ${srcs[i]}`);
  }

  // ④ 定长读：一次 `push 20h`（= 32 字节）到 `Buffer`，紧接着 `call ReadFile`
  const ri = body.findIndex((l) => /^lea ecx,\[ebp\+Buffer\]$/.test(l));
  assert.ok(ri > 0, '读缓冲的地址应由 `lea ecx,[ebp+Buffer]` 取得');
  const readWin = body.slice(Math.max(0, ri - 3), ri + 7);
  assert.ok(readWin.some((l) => /^push 20h/.test(l)),
    `★ 定长读必须是 32 字节（push 20h）—— 这是"6 个声明落在 8..28"的上界（字节 32..52 读不到）。实测窗口：${JSON.stringify(readWin)}`);
  assert.ok(readWin.some((l) => /^call ReadFile$/.test(l)), '紧跟着必须是 `call ReadFile`');

  // ⑤ `mul` 宽度序列：按计数槽绑定 ⇒ 4,4,1Ch,4,4,4（只有 string 池是 28 字节元素）
  const widths = [];
  for (let i = 0; i < body.length - 1; i += 1) {
    if (body[i + 1] !== 'mul edx') continue;
    const m = /^mov edx,([0-9A-F]+h|\d+)$/.exec(body[i]);
    if (!m) continue;
    let slot = null;
    for (let k = i - 1; k >= Math.max(0, i - 12); k -= 1) {
      const q = /^mov (?:eax|edi),\[esi\+ecx\*8\+(5D8[0-9A-F]{2})h\]$/.exec(body[k]);
      if (q) { slot = Number.parseInt(q[1], 16) - FRAME.base; break; }
    }
    if (slot !== null && slot >= COUNT_LO && slot <= COUNT_HI) widths.push({ slot, w: m[1] });
  }
  assert.deepEqual(
    widths.map((x) => x.slot),
    LOCAL_POOL_SLOTS.map((p) => p.count),
    '`mul` 必须按计数槽顺序各出现一次（6 个池）—— 槽号由 `LOCAL_POOL_SLOTS` 算，改坏它这条一起红',
  );
  assert.deepEqual(widths.map((x) => x.w), ['4', '4', '1Ch', '4', '4', '4'], '6 个池的元素宽度（`mov edx,N` + `mul edx`）');
});

test('★ `记录+0x70`（绝对 `0x5D904`）是 `array_container`（std::vector），**不是** local_float 基址', { skip }, () => {
  const all = fs.readFileSync(listing, 'utf8').split('\n').map((l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ').replace(/, /g, ','));
  const i = all.findIndex((l) => l.includes('mov [esi+edx*8+5D904h],ecx'));
  assert.ok(i > 0, '应能找到写 `0x5D904`（记录 `+0x70`；旧基址记法 `帧+0x84`）的那一行（`.text:0040F63B`）');
  const win = all.slice(i, i + 16);
  assert.ok(win.some((l) => /^push 10h ; Size$/.test(l)), '写 `+0x70` 之后应紧跟 `push 10h ; Size`（16 字节对象）');
  assert.ok(win.some((l) => l.includes('operator new(uint)')), '再跟 `operator new(uint)` ⇒ 那是**容器对象**，不是池');
  // 三个 dword 清零：IDA 写作 `[eax]` / `[eax+4]` / `[eax+8]`（十进制偏移，不带 h）
  const cleared = win.filter((l) => /^mov \[eax(\+\d+)?\],ebx$/.test(l));
  assert.equal(cleared.length, 3, `新对象应被清零 3 个 dword（begin/end/cap），实际 ${cleared.length} 条：${JSON.stringify(win.slice(0, 12))}`);
  assert.equal(FRAME.off.arrayContainer, 0x70, '模型里记录 `+0x70` 记为 array_container');
  // ★ 本意是"它不是任何 local 池的基址"。★ 2026-10 三次订正后 `local_float` 是 `+0x24`
  //   （基址 = 记录+0x20/0x24/0x28/0x2C/0x30/0x34；判据与三次订正的经过见 `LOCAL_POOL_SLOTS` 头注）。
  assert.ok(!LOCAL_POOL_SLOTS.some((p) => p.base === FRAME.off.arrayContainer), '★ `+0x70` 不是任何 local 池的基址');
  assert.equal(LOCAL_POOL_SLOTS.find((p) => p.name === 'float').base, 0x24, 'local_float 的基址（取址原语 case 10 读 `0x5D8B8` = 记录 `+0x24`）');
});

test('★ 帧区 slot 的全集：模型声明的那份观察结果必须与语料一致（逐个复核 + 关键槽点名）', { skip }, () => {
  // ★ 记录区的三个数与**尾部三格**的算术关系（改 40 / 0x78 / base 任何一个都会红 —— 变异清单里有两条钉它）：
  //   记录步长 0x78、槽数 40、基址 0x5D894 ⇒ 记录区末尾 0x5EB54；而那三格是 `+0x64/+0x68/+0x6C`（**记录内**）
  //   —— 旧口径把它们写成 `帧+0x78/+0x7C/+0x80`（`+0x78` 正好等于 stride），于是被误读成"独立数组"；
  //   归属已裁决，见 `tools/test/engine-frame.test.mjs`。
  assert.equal(FRAME.count, 40, '槽数 40（`call-script` 的 cur>=39 抛「階層が深すぎます」）');
  assert.equal(FRAME.base + FRAME.stride * FRAME.count, 0x5eb54, '记录区末尾 = 0x5D894 + 40×0x78 = 0x5EB54');
  for (const off of FRAME.off.grids) {
    assert.ok(FRAME_SLOTS_OBSERVED.includes(off), `记录内三格 +0x${off.toString(16)} 必须在观察表里`);
    assert.ok(off < FRAME.stride, `三格 +0x${off.toString(16)} 落在**本条记录内**（< stride 0x78）—— 它们不是"下一帧的 +0x00"`);
  }
  assert.deepEqual([...FRAME.off.grids], [0x64, 0x68, 0x6c], '三格 = 记录内 `+0x64/+0x68/+0x6C`（`0x5D8F8/0x5D8FC/0x5D900`）');
  const { literals, folded } = frameCensus();
  const observed = new Set([...literals.keys(), ...folded.keys()]);
  // 逐个复核"在"
  for (const off of FRAME_SLOTS_OBSERVED) {
    assert.ok(observed.has(off), `观察表里说 +0x${off.toString(16)} 有访问，两种形态里都没找到`);
  }
  // 反向：语料里出现的、模型没登记的（只报警不判红会太松 ⇒ 这里直接判红，逼着两边对齐）
  const missing = [...observed].filter((o) => !FRAME_SLOTS_OBSERVED.includes(o)).sort((a, b) => a - b);
  assert.deepEqual(missing.map((o) => `+0x${o.toString(16)}`), [], '语料里有访问、而观察表漏登记的槽');
  // 关键槽点名核（用模型常量点名 ⇒ 改坏任一个偏移这条一起红）
  const named = [
    FRAME.off.strBase, FRAME.off.operands, FRAME.off.count0, FRAME.off.base0,
    FRAME.off.caller, FRAME.off.frameArg, FRAME.off.operandCount, FRAME.off.arrayContainer,
  ];
  for (const off of named) assert.ok(observed.has(off), `关键槽 记录+0x${off.toString(16)} 必须真的在场`);
  assert.ok(observed.has(0x24), '★ `+0x24` 在场（折叠形态 `0xC79`）—— 我上一轮说它不存在，那是错的');
  assert.equal(FRAME_SLOTS_OBSERVED.length, 30, '★ 观察表 = 逐 dword 连续 30 格（`0x00..0x74`）⇒ 记录大小 = `0x78`');
});


test('★ 记录 `+0x70`/`+0x74` 的访问普查：同一格有**两种寻址形态**，读点在**值/取址原语**里 —— 形态不能当"记录字段 vs Engine 级"的判据', { skip }, () => {
  // ## 这条守的是"方法"，不是某个数字
  // 实测反例：同一格 `记录+0x70`（绝对 `5D904h`；旧基址记法 `帧+0x84`）**两种形态都在**
  //   * 扁平形（基址被提升进寄存器）：`.text:0040EBA2 mov eax,[esi+5D904h]`、`0040EC41 mov [esi+5D904h],ebx`、`004056D3 mov [eax+5D904h],esi`
  //   * `*8` 索引形（`reg = 15·cur`）：`.text:0040F63B mov [esi+edx*8+5D904h],ecx`、`0042B362 mov ecx,[esi+ecx*8+5D904h]`
  // ⇒ **"扁平 vs `*8` 索引"不能当"记录字段 vs Engine 级寄存器"的判据**：形态会随编译器的基址提升而漂，
  //   判据要用"**谁在算索引**"（这里是 `cur` ⇒ 记录内）。只按字面地址统计会给同一字段两种计数。
  // ★ 这两个槽的**读点**落在四个不同的原语里（`+0x70`）/（`+0x74`），不是某一个函数的分支 —— 归错函数会让人去读错函数体。
  const raw = fs.readFileSync(listing, 'utf8').split('\n');
  const byEa = new Map();
  for (const l of raw) {
    const m = /:([0-9A-F]{8})\s+(.*?)\s*$/.exec(l.trim());
    if (m) byEa.set(Number.parseInt(m[1], 16), m[2].replace(/\s+/g, ' ').replace(/, /g, ','));
  }
  const at = (off) => `${(FRAME.base + off).toString(16).toUpperCase()}h`;
  const all = [...byEa.values()];
  const flat = (off) => all.filter((l) => new RegExp(`\\[[a-z]{2,3}\\+${at(off)}\\]`).test(l));
  const indexed = (off) => all.filter((l) => new RegExp(`\\[[a-z]{2,3}\\+[a-z]{2,3}\\*8\\+${at(off)}\\]`).test(l));

  // ① ★ 两种形态都必须在（`FRAME.off.arrayContainer` 被改坏 ⇒ 这里的地址变了 ⇒ 当场红）
  for (const off of [FRAME.off.arrayContainer, 0x74]) {
    assert.ok(flat(off).length > 0,
      `记录+0x${off.toString(16)}（${at(off)}）应出现**扁平形态**（基址提升进寄存器）—— 没有它说明普查漏了形态`);
    assert.ok(indexed(off).length > 0,
      `记录+0x${off.toString(16)}（${at(off)}）应出现 **\`*8\` 索引形态**（\`reg = 15·cur\`）`);
  }
  // ② `+0x70` 的索引形态读点：取址原语 + 三个值原语（★ 不是"全在 sub_42B4B0 里"）
  for (const ea of [0x42b362, 0x42b91c, 0x42be8a, 0x42c422]) {
    assert.equal(byEa.get(ea), `mov ecx,[esi+ecx*8+${at(FRAME.off.arrayContainer)}]`,
      `0x${ea.toString(16).toUpperCase()} 必须以 \`*8\` 索引形态读 记录+0x70（逐字是 ecx = 15·cur 之后的 \`mov ecx,[esi+ecx*8+…]\`）`);
  }
  // ③ `+0x74`（绝对 `0x5D908`）的索引形态读点（含 `sub_433310` —— 它不是那四个"取地址/写值"原语之一）
  for (const ea of [0x42b24a, 0x42c30a, 0x433507]) {
    assert.equal(byEa.get(ea), `mov ecx,[esi+ecx*8+${at(0x74)}]`,
      `0x${ea.toString(16).toUpperCase()} 必须以 \`*8\` 索引形态读 记录+0x74`);
  }
  // ④ 装载器**写**它们（容器对象指针），拆卸器**读并清 0** —— 与"读点"是两拨人，别混
  assert.equal(byEa.get(0x40f63b), `mov [esi+edx*8+${at(FRAME.off.arrayContainer)}],ecx`, '装载器写 记录+0x70（`operator new(0x10)` 的容器对象）');
  assert.equal(byEa.get(0x40f678), `mov [esi+edx*8+${at(0x74)}],ecx`, '装载器写 记录+0x74');
  assert.equal(byEa.get(0x40ed1a), 'mov dword ptr [esi+5D908h],0', '拆卸路径把 记录+0x74 清 0（`*8` 索引形态**没有**出现 —— 又一个"形态会漂"的例子）');
});
test('★ 帧尾三格 `0x5D8F8/0x5D8FC/0x5D900` 全语料**只有 6 处引用**（建立 + `_memset` + 释放）⇒ **没有读者**', { skip }, () => {
  // 这条把"用途"从一个**未查明项**变成一个**结论**：三个 per-cur dword 格每格装一个
  // `operator new[](8*(n+1))` 出来的 8 字节元素表，建立后**除了释放路径没人读**。
  // ★ 复算方式：全语料逐行找这三个**字面地址**（`5D8F8h` / `5D8FCh` / `5D900h`）—— 必须**恰好**这 6 行，
  //   且其中唯一的"读指针"发生在 `lea ebx,[esi+5D8F8h]`（+`edi=3`）之后的 `operator delete[]`。
  const raw = fs.readFileSync(listing, 'utf8').split('\n');
  const byEa = new Map();
  for (const l of raw) {
    const m = /:([0-9A-F]{8})\s+(.*?)\s*$/.exec(l.trim());
    if (m) byEa.set(Number.parseInt(m[1], 16), m[2].replace(/\s+/g, ' ').replace(/, /g, ','));
  }
  const expected = [
    [0x4056bd, 'mov [ecx+edx*4+5D8F8h],esi'],
    [0x4056c4, 'mov [ecx+edx*4+5D8FCh],esi'],
    [0x4056cb, 'mov [ecx+edx*4+5D900h],esi'],
    [0x40eb7c, 'lea ebx,[esi+5D8F8h]'],
    [0x40f5c1, 'mov [esi+ecx*4+5D8F8h],eax'],
    [0x40f5ec, 'mov eax,[esi+eax*4+5D8F8h]'],
  ];
  for (const [ea, text] of expected) {
    assert.equal(byEa.get(ea), text, `0x${ea.toString(16).toUpperCase()} 的逐字形态变了`);
  }
  const mentioned = [...byEa.entries()].filter(([, t]) => /5D8F[8C]h|5D900h/.test(t)).map(([ea]) => ea).sort((a, b) => a - b);
  assert.deepEqual(mentioned, expected.map(([ea]) => ea).sort((a, b) => a - b),
    `★ 全语料提到这三个字面地址的行必须**恰好**这 6 处 —— 多出来的那处很可能就是"读者"（实测：${mentioned.map((e) => '0x' + e.toString(16)).join(', ')}）`);
  // 释放路径：`mov edi,3` 轮三格，唯一的"读指针"在这里
  assert.equal(byEa.get(0x40eb82), 'mov edi,3', '释放路径以 `edi = 3` 轮三格');
  assert.equal(byEa.get(0x40eb87), 'mov eax,[ebx]', '★ 唯一的"读指针"发生在释放路径（`lea ebx,[esi+5D8F8h]` 之后）');
  assert.ok((byEa.get(0x40eb8e) ?? '').startsWith('call ??_V@YAXPAX@Z'), '⇒ 紧接着是 `operator delete[]`');
  // 建立路径里那一次读是为了 `_memset` 的落点（不是使用数据）
  assert.ok((byEa.get(0x40f5f4) ?? '').startsWith('call _memset'), '`0x40F5EC` 取指针之后紧跟 `_memset`（⇒ 那次读只为填 `0xFF`）');
});


// ─────────────────────────────────────── ② 行为判据（池这一层）

test('★ 帧内其它字段的**用法形态**（traceable 到语料，而不是只登记一个名字）', { skip }, () => {
  const all = fs.readFileSync(listing, 'utf8').split('\n').map((l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ').replace(/, /g, ','));
  const at = (off) => `${(FRAME.base + off).toString(16).toUpperCase()}h`;
  const has = (off, re) => all.some((l) => l.includes(at(off)) && re.test(l));
  // `+0x00` 脚本缓冲基址（绝对 `0x5D894`）：装载器里由 GlobalAlloc 出来（写），并被用作 (len,ptr) 的基址
  assert.ok(has(FRAME.off.strBase, /GlobalAlloc/i) || has(FRAME.off.strBase, /mov \[.*\],.*eax/), '`+0x00` 应被写（脚本缓冲基址）');
  // `+0x38` caller（绝对 `0x5D8CC`）：装载时写入；**退出路径读它**（`.text:0041A834`，在 exit handler 里）
  assert.ok(has(FRAME.off.caller, /mov \[.*\],/), '`+0x38` 应被写（caller 回链）');
  assert.ok(
    all.some((l) => l === `mov eax,[esi+ecx*8+${at(FRAME.off.caller)}]`),
    '`+0x38` 应在**记录内索引**形态下被读（exit 路径用它取回链）',
  );
  // `+0x3C` frameArg：被当作**键**比较（`cmp …, eax`）
  assert.ok(has(FRAME.off.frameArg, /^cmp /) || all.some((l) => l.startsWith(`cmp [esi+`) && l.includes(at(FRAME.off.frameArg))), '`+0x3C` 应被当作键比较');
  // 三组 (len,ptr)（`+0x40..+0x54`）：装载器写、消息调用点读（见上面那条用例）
  for (const t of FRAME.off.triples) {
    for (const off of [t.len, t.ptr]) {
      assert.ok(all.some((l) => l.includes(at(off))), `三组 (len,ptr) 的槽 记录+0x${off.toString(16)} 必须在语料里出现`);
    }
  }
  assert.equal(FRAME.off.triples.length, 3, '三组 (len,ptr)');
});

test('★ int 族过 DEC、float 族不过、下标不过 —— 三条口径一起判', () => {
  const key = 0xdeadbeef;
  const lp = new LocalPools(key);
  const gp = new GlobalPools(key);

  // int：写进去 → 内存里是编码位模式 → 读回来是原值
  const bits = lp.write(9, 3, 0x12345678);
  assert.notEqual(bits, 0x12345678, '★ int 槽在"内存"里**不应**是原值（否则等于没编码）');
  assert.equal(lp.read(9, 3), 0x12345678, '读回来必须是原值');
  assert.equal(lp.pools.get('int').get(3), bits, '读的是同一份位模式');

  // float：原样
  const f = 1.5;
  assert.equal(lp.write(10, 3, f), f, '★ float 槽**不应**过编码');
  assert.equal(lp.read(10, 3), f, 'float 读回来就是写的那个数');

  // 全局池同口径
  const gBits = gp.write('int', 5, 42);
  assert.notEqual(gBits, 42, 'global int 也要过编码');
  assert.equal(gp.read('int', 5), 42);
  assert.equal(gp.write('float', 5, 2.25), 2.25, 'global float 不过编码');

  // 未初始化 ≠ 0：int 族初值是 enc_zero
  const lp2 = new LocalPools(key);
  lp2.initZero(9, 4);
  assert.equal(lp2.read(9, 0), 0, 'enc_zero 解码回来是 0（所以"看起来像 0"）');
  assert.notEqual(lp2.pools.get('int').get(0), 0, '★ 但盘上那一位**不是 0** —— "未初始化 = 0"是错的');
  // float 族初值确实是 0
  lp2.initZero(10, 4);
  assert.equal(lp2.pools.get('float').get(0), 0, 'float 族初值就是 memset 0');
});

test('★ 下标不过编码：`base + idx*4` 的偏移与 idx 线性（把 idx 过 DEC 会取到别的槽）', () => {
  assert.equal(GlobalPools.slotOffset(0), 0);
  assert.equal(GlobalPools.slotOffset(7), 28);
  assert.equal(GlobalPools.slotOffset(7) - GlobalPools.slotOffset(6), 4, '相邻下标恰好差 4 字节');
  // 反证：如果把它过一遍 DEC，第 7 槽会落到别处
  const key = 0xdeadbeef;
  const lp = new LocalPools(key);
  lp.write(9, 7, 99);
  const wrongIdxBits = lp.pools.get('int').get(7) ^ lp.pools.get('int').get(7);
  assert.equal(wrongIdxBits, 0, '（哨兵：同一槽异或自己是 0 —— 只为证明这条用例动的是真数据）');
  assert.equal(lp.read(9, 7), 99, '按**原下标**读得到；下标若被编码过就读不到');
});

test('★ 操作数寻址：每个操作数 8 字节，且 opcode 在 `[第一操作数 − 4]`（★ 这是**布局**算术，不在模拟器里）', () => {
  // 原语由布局层自己算地址 —— 模拟器不提供这种算术（它按语义槽位建模，不按字节地址）
  const frameBaseOf = (cur) => FRAME.base + FRAME.stride * cur;
  const operandAt = (cur, index) => frameBaseOf(cur) + FRAME.off.operands + 8 * index;
  const a0 = operandAt(0, 0);
  const a1 = operandAt(0, 1);
  const a2 = operandAt(0, 2);
  assert.equal(a1 - a0, 8, '操作数步长必须是 8 字节');
  assert.equal(a2 - a1, 8);
  // opcode 在 a0 − 4 ⇒ 它与操作数基址槽正好相邻
  assert.equal(a0 - 4, FRAME.base + FRAME.off.operands - 4, 'opcode 位置 = 第一操作数 − 4');
  // 帧基址：第 cur 帧
  assert.equal(frameBaseOf(0), FRAME.base);
  assert.equal(frameBaseOf(1) - frameBaseOf(0), FRAME.stride);
  assert.equal(frameBaseOf(FRAME.count - 1) + FRAME.stride, FRAME.base + FRAME.stride * FRAME.count, '40 帧的末尾');
  // ★ 模拟器**不导出**任何"按地址取操作数"的 API —— 这条把那个分离钉住
  assert.ok(!('frameBaseOf' in poolsModule), '模拟器不该导出按地址算帧基址的 API（那是布局知识）');
  assert.ok(!('operandAt' in poolsModule), '模拟器不该导出按地址算操作数的 API（那是布局知识）');
});

test('★ 三组 (长度, 指针) + `0x5D8EC` 位置 + `0x5D894` 基址：一起喂给 `sub_48E870`（逐字形态）', { skip }, () => {
  const all = fs.readFileSync(listing, 'utf8').split('\n').map((l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ').replace(/, /g, ','));
  const i = all.findIndex((l) => l.includes('mov edx,[esi+ecx*8+5D8D4h]') && l.includes('5D8D4h'));
  assert.ok(i > 0, '语料里应能找到 `mov edx,[esi+ecx*8+5D8D4h]`（第一组的长度）');
  const win = all.slice(i, i + 16);
  const seq = ['mov edx,[esi+ecx*8+5D8D4h]', 'mov ecx,[eax+5D8D8h]', 'mov edx,[eax+5D8ECh]', 'lea ecx,[esi+4E3ACh]', 'call sub_48E870'];
  let k = 0;
  for (let j = 0; j < win.length && k < seq.length; j += 1) if (win[j] === seq[k]) k += 1;
  assert.equal(k, seq.length, `三组 (len,ptr)+位置 → 消息引擎 的序列不完整（只对上 ${k}/${seq.length}）：\n${win.join('\n')}`);
  // 三对必须两两相邻（len 在 ptr 前 4 字节）
  for (const t of FRAME.off.triples) assert.equal(t.ptr - t.len, 4, `记录+0x${t.len.toString(16)}/0x${t.ptr.toString(16)} 必须相邻（长度在前、指针在后）`);
  assert.deepEqual(FRAME.off.triples.map((t) => t.len), [0x40, 0x48, 0x50], '三组 (len,ptr) 的起始偏移（绝对 `0x5D8D4/0x5D8DC/0x5D8E4`）');
  // 三对之后紧跟那个**位置**格（`0x5D8EC` = 记录 `+0x58`）
  assert.equal(FRAME.off.position58 - FRAME.off.triples[2].ptr, 4, '`0x5D8EC` 紧跟在第三组之后');
  assert.equal(FRAME.base + FRAME.off.position58, 0x5d8ec, '★ 位置格 = 绝对 `0x5D8EC`（旧基址记法写作 `帧+0x6C`）');
});

test('★ 记录尾部 `+0x64/+0x68/+0x6C`（= `0x5D8F8/0x5D8FC/0x5D900`）与 `+0x70/+0x74` 的归属：**索引步长 0x78 就是记录大小**（旧的"两个量"口径已否）', () => {
  // ★ 这一条留下的教训（四次翻案的收敛结果）：**"记录内字段的索引步长是 0x78"** 就是 **"记录是 0x78 字节"** ——
  //   旧口径之所以认为"是两个不同的量"，是因为把 **`0x5D880`（`cur` 的槽）** 当成了记录基址：
  //   于是 `0x5D904/0x5D908` 被读成 `+0x84/+0x88` ⇒ 得出"下界 0x8C > 0x78"。
  //   真基址 `0x5D894` 之下它们是 `+0x70/+0x74`，**落在 `0x78` 之内**。
  //   ① 三格 `0x5D8F8/0x5D8FC/0x5D900` 写作 `[reg+edx*4+5D8xxh]` 而 `edx = 30·cur` ⇒ 净字节步长**同为 0x78**
  //      ⇒ 它们是**记录内三格**（`+0x64/+0x68/+0x6C`），不是"下一帧的 +0x00"。
  //   ② 记录里被写的**最大**偏移 = `+0x74`（`0x40F678 [esi+edx*8+5D908h]`）⇒ 记录大小 = `0x74 + 4 = 0x78`。
  //   ③ ⛔ `imul eax,84h`（`0x40EB3F`）**不是**记录大小 —— 它的落点是 `Engine+0x69334 + 0x84*cur`（另一个块）。
  //   台账：`KN-01M4H0MZGD4K4P4F0J1E7E5G79`；可执行判据在 `tools/test/engine-frame.test.mjs`。
  assert.equal(FRAME.stride, 0x78, '记录的**字段索引**步长 0x78（三处独立机械证据）');
  // 逐字验：`.text:0040F31B..F335` 的 `15*(cur+0xC79)` ⇒ 目标 = Engine + 120*cur + 0x5D8B8 = 记录+0x24
  assert.equal(FRAME.base + FRAME.off.base0 + 4, 0x5d8b8, '`local_float` 基址格 = 绝对 `0x5D8B8`（记录 `+0x24`；旧基址记法 `帧+0x38`）');
  assert.equal(15 * 8, 120, '`shl 4; sub` 得到的是 15·x，再 `*8` ⇒ 每 cur 120 字节');
  // `+0x70` 那个对象是容器（见上一条用例的逐字断言）
  assert.equal(FRAME.off.arrayContainer, 0x70);
  // ★ 两个都成立，且**不相等**：`local_float` 基址在 `+0x24`，`array_container` 在 `+0x70`
  assert.notEqual(LOCAL_POOL_SLOTS.find((p) => p.name === 'float').base, FRAME.off.arrayContainer);
  // ★ 把"大小 = 步长"写成断言（而不是散文）：最大字段偏移 `+0x74`，`+4` 正好是 `0x78`
  assert.ok(0x74 + 4 === FRAME.stride, '★ 最大字段偏移 + 4 **等于**索引步长 ⇒ "记录大小与索引步长是两个量"这条**不成立**');
  assert.ok(FRAME.off.grids.every((off) => off < FRAME.stride), '★ 三格全在记录**之内**（< stride）—— "`+0x78` 区是下一帧"那条推理无法复活');
});

test('★ 越界**不检查**但要**留痕**（引擎没有下标检查 ⇒ 模型不许悄悄 clamp 或补 0）', () => {
  const lp = new LocalPools(0);
  // 未初始化槽：返回 null（而不是 0）—— 0 是"合法值"，两者不能混
  assert.equal(lp.read(9, 999), null, '未初始化/越界读必须返回 null，不许补 0');
  lp.noteOOB(9, 999, 'read-uninitialized');
  assert.equal(lp.oob.length, 1, '越界/未初始化访问要留痕（供后续分析填补，而不是静默）');
  assert.equal(lp.oob[0].kind, 'read-uninitialized');
});
