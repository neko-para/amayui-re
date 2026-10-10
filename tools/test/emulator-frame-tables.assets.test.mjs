/** @env assets @kind contract @why 三张表不再等于对应 opcode 的位置序列 / 不再相邻 / 表长度与出现次数不符 ⇒ "表紧跟在指令区之后"这条边界口径与"表项是位置"这条读数同时失真 */
/**
 * tools/test/emulator-frame-tables.assets.test.mjs —— **指令区之后的三张表**（批 R1 迭代点 ⑥ 的续）
 *
 * ## 它守的是什么（一句话）
 * 头部 `+36/+40`、`+44/+48`、`+52/+56` 那三组 `(长度, 偏移)` 指向的**三张 u32 数组**：
 * 它们在 491 个脚本上**恒相邻且序恒为 t1→t2→t3**，并且
 * **`table_i[k]` 逐元素等于「第 k 条 opcode 为该类的那条指令」的 dword 位置**、**表长度 == 该类 opcode 的出现次数**。
 *
 * ## 三种表 ↔ 三种 opcode（★ 逐组由访问点定，**不按顺序猜**）
 * | 表 | 头字段 | 对应 opcode | 语料规模 |
 * |---|---|---|---|
 * | `table_1` | `+36/+40` | `0x71` | 90703 项 |
 * | `table_2` | `+44/+48` | `0x03`（`call-script`） | 7588 项 |
 * | `table_3` | `+52/+56` | `0x8f`（`call`） | 150781 项 |
 *
 * ## ★ 命名冲突（显式化，**不许静默改名**）
 * 需求单 `REQ-01M48FRMKME8VERSH42M7MQMYP` 的标题写的是「三张表（**label** / message / call）」，
 * 而实测是 **`0x71` / `0x3 call-script` / `0x8f call` 的**位置表**：
 * * 旧仓那半句（`0x71` 消息表 / `0x3` call-script 表 / `0x8F` call 表）**成立**（本文件就是它的机械化）；
 * * 「`table_1` = **通用 label 表**」**不成立** —— table_1 的 90703 项 100% 落在 `0x71` 指令上，
 *   语料里**没有**一张"通用 label 表"；
 * * 「记录 `+0x58`（绝对 `0x5D8EC`；旧基址记法 `帧+0x6C`）是**下标**」**不成立** —— 它存的是**位置**
 *   （见台账 `Engine+0x5D880/frame-0x6C-0x70-position-not-index`）；
 *   本文件里"表项处就是该类 opcode 本身"这条断言就是"表项是位置"的机械判据。
 *
 * ## 为什么锚 EA（不是行号）
 * ★ 记法：下面 `记录+0xNN` 相对 `FRAME_LAYOUT.base = 0x5D894`；括号里的 `帧+0xNN` 是**旧基址（0x5D880）记法**。
 * * `0x41EDD7`（`sar edx,2`）/ `0x41EDDA`（`mov [eax+5D8ECh],edx`）⇒ `记录+0x58（帧+0x6C） = (ip − 脚本基址) >> 2`；
 * * `0x41EE44/0x41EE4E/0x41EE55/0x41EE63` ⇒ `sub_48E870(消息引擎, 记录+0x58（帧+0x6C）, 记录+0x44（帧+0x58）, 记录+0x40（帧+0x54）)`（键、表指针、长度）；
 * * `0x48E8A9`（`cmp [ebx+eax*4],edx`）⇒ `sub_48E870` 是"在表里线性查找那个**位置**"、返回**下标**或 `-1`；
 * * `0x419381/0x419387/0x419390/0x419393`（恢复路径）⇒ `PC = 记录+0x00（帧+0x14） + 4*table_2[k]`（**表项当代码位置用**）。
 *
 * ## 运行
 * `pnpm test:assets`（要 `dist/install` 里的脚本语料；缺席时跳过，**不**静默通过）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { OPCODE_TABLE, readHeader } from '@amayui/age-format/src/asm/runtime.mts';
import { iterate } from '../../apps/emulator/src/model/iterate.ts';

const BIN_DIR = path.join(REPO_ROOT, 'dist', 'install');
const bins = fs.existsSync(BIN_DIR)
  ? fs.readdirSync(BIN_DIR).filter((f) => f.toLowerCase().endsWith('.bin')).sort()
  : [];
const skip = bins.length ? false : '没有 .BIN 语料（先 `pnpm tools release install`）';
const table = OPCODE_TABLE;

/** 三张表 ↔ 三种 opcode 的**唯一真源**（本文件的判据全从它派生） */
const CLASSES = [
  { key: 'table_1', opcode: 0x71, lenField: 'table_1_length', offField: 'table_1_offset' },
  { key: 'table_2', opcode: 0x03, lenField: 'table_2_length', offField: 'table_2_offset' },
  { key: 'table_3', opcode: 0x8f, lenField: 'table_3_length', offField: 'table_3_offset' },
];

/** 本语料的子头块长度（头 `+32`）：491/491 都是 28 ⇒ 代码区起点 = `32 + 28 = 60 = headerLen` */
const SUB_HEADER_LENGTH = 28;

/** 一次扫描：只收 `SYS4` 脚本（`SYS4INI.BIN` 是 `S4IC` 配置，不是脚本） */
const scripts = [];
for (const name of bins) {
  const bin = fs.readFileSync(path.join(BIN_DIR, name));
  if (!bin.subarray(0, 4).equals(Buffer.from('SYS4', 'latin1'))) continue;
  const header = readHeader(bin);
  scripts.push({ name, bin, header, iter: iterate(bin, { table }) });
}

/**
 * 读第 i 张表（u32 数组，基址 = `headerLen + 4*偏移`，项数 = 长度字段）。
 * ★ 越界时**响亮失败**并说明是哪一对 (长度, 偏移) —— 不静默返回半张表，也不让 `readUInt32LE` 抛裸的 `ERR_OUT_OF_RANGE`
 *   （"三组 (len,off) 配对抄错一格"正是这套断言要抓的破坏）。
 */
function readTable(s, c) {
  const off = s.header.fields[c.offField];
  const len = s.header.fields[c.lenField];
  if (!Number.isInteger(off) || !Number.isInteger(len) || off < 0 || len < 0 || s.header.length + 4 * (off + len) > s.bin.length) {
    throw new Error(
      `${s.name}: ${c.key} 的 (长度=${len}, 偏移=${off}) 越出文件（${s.bin.length} 字节，headerLen=${s.header.length}）` +
        ' —— 三组 (长度, 偏移) 的配对或字段序与语料不符',
    );
  }
  const out = [];
  for (let k = 0; k < len; k += 1) out.push(s.bin.readUInt32LE(s.header.length + 4 * off + 4 * k));
  return out;
}

test('★ 三张表恒相邻且序恒为 t1→t2→t3：`off_i + len_i == off_{i+1}`（491 脚本逐条核）', { skip }, () => {
  const bad = [];
  let checked = 0;
  for (const s of scripts) {
    checked += 1;
    const g = CLASSES.map((c) => ({ c, off: s.header.fields[c.offField], len: s.header.fields[c.lenField] }));
    if (g[0].off + g[0].len !== g[1].off) bad.push(`${s.name}: off1+len1=${g[0].off + g[0].len} ≠ off2=${g[1].off}`);
    if (g[1].off + g[1].len !== g[2].off) bad.push(`${s.name}: off2+len2=${g[1].off + g[1].len} ≠ off3=${g[2].off}`);
    for (const { c, len } of g) {
      if (s.header.length + 4 * (s.header.fields[c.offField] + len) > s.bin.length) bad.push(`${s.name}: ${c.key} 越过文件尾`);
    }
  }
  assert.deepEqual(bad, [], `三张表不再恒相邻（序必须是 t1→t2→t3）：\n  - ${bad.slice(0, 12).join('\n  - ')}`);
  assert.ok(checked >= 480, `脚本语料应有 480+ 个（实测 491），实际检查了 ${checked} 个`);
});

test('★ `table_i` 逐元素等于该类 opcode 指令的 dword 位置序列、且表长度 == 出现次数（491 脚本逐条核）', { skip }, () => {
  const bad = [];
  let pairs = 0;
  for (const s of scripts) {
    const hl = s.header.length;
    const positions = new Map();
    for (const ins of s.iter.instructions) {
      const p = (ins.byteOffset - hl) / 4;
      if (!Number.isInteger(p)) bad.push(`${s.name}: 指令位置非 4 对齐（byteOffset=${ins.byteOffset}）`);
      if (!positions.has(ins.opcode)) positions.set(ins.opcode, []);
      positions.get(ins.opcode).push(p);
    }
    for (const c of CLASSES) {
      pairs += 1;
      const vals = readTable(s, c);
      const want = positions.get(c.opcode) ?? [];
      if (vals.length !== want.length) {
        bad.push(`${s.name}/${c.key}: 表长度 ${vals.length} ≠ opcode 0x${c.opcode.toString(16)} 的出现次数 ${want.length}`);
        continue;
      }
      for (let k = 0; k < vals.length; k += 1) {
        if (vals[k] !== want[k]) {
          bad.push(`${s.name}/${c.key}[${k}]: 表里 ${vals[k]} ≠ 第 ${k} 条 0x${c.opcode.toString(16)} 的位置 ${want[k]}`);
          break;
        }
      }
    }
  }
  assert.deepEqual(
    bad,
    [],
    '★ 三张表不再等于对应 opcode 的位置序列 ⇒ "表项 = 该类指令的 dword 位置"这条口径与语料不符（' +
      '它同时是"表紧跟在指令区之后"那条边界口径的判据）：\n  - ' + bad.slice(0, 12).join('\n  - '),
  );
  assert.ok(pairs >= 480 * 3, `检查的 (脚本, 表) 对数应接近 491×3，实际 ${pairs}`);
});

test('★ 头 +32 的子头块长度恒 28 ⇒ 代码区起点 32+28 == headerLen；表项是**位置**（`headerLen + 4*值` 处就是该 opcode）', { skip }, () => {
  const bad = [];
  const subHist = new Map();
  let entries = 0;
  for (const s of scripts) {
    const sub = s.header.fields.sub_header_length;
    subHist.set(sub, (subHist.get(sub) ?? 0) + 1);
    if (32 + sub !== s.header.length) bad.push(`${s.name}: 32+sub(${sub}) ≠ headerLen(${s.header.length})`);
    // ★ "表项是位置（不是下标）"的机械判据：值 v ⇒ 字节 `headerLen + 4*v` 处就是该 opcode 本身
    for (const c of CLASSES) {
      for (const v of readTable(s, c)) {
        entries += 1;
        const at = s.header.length + 4 * v;
        if (at + 4 > s.bin.length || s.bin.readUInt32LE(at) !== c.opcode) {
          bad.push(`${s.name}/${c.key}: 表项 ${v} ⇒ 偏移 ${at} 处不是 opcode 0x${c.opcode.toString(16)}`);
          break;
        }
      }
    }
  }
  assert.deepEqual(bad, [], `表项不再落在该类 opcode 的指令起点（"位置"这条读数失效）：\n  - ${bad.slice(0, 12).join('\n  - ')}`);
  assert.deepEqual([...subHist.keys()], [SUB_HEADER_LENGTH], `子头块长度本语料恒 ${SUB_HEADER_LENGTH}，实测分布 ${JSON.stringify([...subHist])}`);
  assert.ok(entries > 200_000, `三个表的表项总数应超过 20 万（实测 249072），实际 ${entries}`);
});
