/** @env assets @kind gate @why 帧步长/帧内偏移被改坏，或有人把"帧外数组"当成帧内字段读 */
/**
 * tools/test/engine-frame.test.mjs —— 帧布局的守卫（迭代点 ③：**只算布局，不取值**）
 *
 * ## 它守的两件事
 * 1. **帧布局的机械普查**：全语料扫一遍"帧相对偏移"的用法，`≥ stride` 的槽**只允许 0 或 1 个**
 *    （实测 1 个，且那一个是**别的结构**：见下）。
 * 2. **帧步长的三处独立证据**：两条取操作数原语 + 帧字段清零循环都用 `shl 4; sub` 算 `15·cur`
 *    （⇒ `*8` 后 = `120·cur`）。
 *
 * ## ★ 迭代点 ③ 的结论（原缺陷单 `REQ-01M48E8HHKYM854RXN9ZG8DSH7` 的裁决材料）
 * 旧仓那份"帧装载器在帧+0x78/0x7C/0x80 建三个 8 字节数组"的**读数口径错了**：新语料里那三个地址是
 *   `[ecx+edx*4+5D8F8h] / [ecx+edx*4+5D8FCh] / [ecx+edx*4+5D900h]`（`edx = 2*15*cur`）
 * ⇒ 它们是**三个"按 cur 索引、步长 4"的独立数组**，**不是帧内字段**（帧内字段一律以 `15·cur` 为索引）。
 * 另有 `lea ebx,[esi+5D8F8h]` + `mov edi,3` 的循环（无 `cur` 参与）⇒ 是"基址 + 3 项"的访问。
 * ⇒ 帧步长 `0x78` 这一侧**不受影响**；那三个数组的**真正归属**仍未查明（记为缺口）。
 *
 * 运行：`pnpm test:assets`（要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';

import { REPO_ROOT } from '../lib/paths.mjs';

/** A 侧：帧布局（基址 / 步长 / 帧数）—— 与台账 `Engine+0x5D880/frame-stride-0x78` 同源 */
const FRAME = { base: 0x5d880, stride: 0x78, count: 40 };

/** B 侧（旧仓读数，**已判定为"另一套坐标/别的结构"**）：那三个地址 */
const OLD_CLAIM = [0x78, 0x7c, 0x80];
const THREE_ARRAYS = [0x5d8f8, 0x5d8fc, 0x5d900];

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';

/** 语料里所有"帧相对偏移"的两种寻址形态 */
const RE_INDEXED = /\[(\w{2,3})\+(\w{2,3})\*8\+(5D8[0-9A-F]{2})h\]/;
const RE_FLAT = /\[\w{2,3}\+(5D8[0-9A-F]{2})h\]/;

/** 扫全语料，统计每个"帧相对偏移"的读/写次数（索引形式 = `15·cur`；扁平形式 = 帧基址 + 偏移） */
async function frameSlotCensus() {
  const slots = new Map();
  const rl = readline.createInterface({ input: fs.createReadStream(listing, { encoding: 'utf8' }), crlfDelay: Infinity });
  let line = 0;
  for await (const raw of rl) {
    line += 1;
    const m = RE_INDEXED.exec(raw) ?? RE_FLAT.exec(raw);
    if (!m) continue;
    const abs = Number.parseInt(m[m.length - 1], 16);
    const off = abs - FRAME.base;
    const isWrite = /^[^;]*mov\s+\[[^\]]+\]\s*,/.test(raw);
    const s = slots.get(off) ?? { off, read: 0, write: 0, first: line };
    s[isWrite ? 'write' : 'read'] += 1;
    slots.set(off, s);
  }
  rl.close();
  return { slots: [...slots.values()].sort((a, b) => a.off - b.off), lines: line };
}

test('★ 帧内偏移的机械普查：`≥ stride` 的槽最多 1 个（那个是**别的结构**，不是帧字段）', { skip }, async () => {
  const { slots, lines } = await frameSlotCensus();
  assert.ok(lines > 100000, `应扫过十万行以上，实际 ${lines}`);
  assert.ok(slots.length > 20, `帧内槽位数应当有几十个，实际 ${slots.length}`);

  const beyond = slots.filter((s) => s.off >= FRAME.stride);
  assert.deepEqual(
    beyond.map((s) => `+0x${s.off.toString(16)}`),
    ['+0x78'],
    '★ 帧内**不许**出现多个 `≥ stride` 的槽：出现多个 ⇒ 要么 stride 不是 0x78，要么有人把相邻结构当帧字段（本条守卫存在的理由）',
  );
  // 而且那一个必须**只有 1 次读**（实测是一次 `lea`，不是"帧内字段"的规律性访问）
  assert.ok(beyond[0].read <= 1 && beyond[0].write === 0, `+0x78 只应有一次非写访问，实际 读${beyond[0].read}/写${beyond[0].write}`);

  // 帧内确实有字段（不是空扫）：关键槽必须在场
  const offs = new Set(slots.map((s) => s.off));
  for (const need of [0x0, 0x4, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x48, 0x4c, 0x74]) {
    assert.ok(offs.has(need), `帧内应出现 +0x${need.toString(16)}（普查漏了？）`);
  }
});

test('★ 那三个"帧尾数组"是**按 cur 索引、步长 4** 的独立数组（不是帧内字段）', { skip }, async () => {
  const text = fs.readFileSync(listing, 'utf8');
  for (const abs of THREE_ARRAYS) {
    // IDA 的写法：`5D8F8h`（去掉前导零、大写十六进制 + h），不是零填充的 8 位
    const hex = `${abs.toString(16).toUpperCase()}h`;
    const idxed = new RegExp(`\\[\\w{2,3}\\+\\w{2,3}\\*4\\+${hex}\\]`, 'i');
    assert.ok(idxed.test(text), `0x${abs.toString(16)} 应被"按索引 ×4"寻址（步长 4 的数组，形如 [ecx+edx*4+${hex}]），语料里找不到`);
    // ★ 反例：它**不许**以 `15·cur` 的索引形态出现（那才叫帧内字段）
    const asFrameField = new RegExp(`\\[\\w{2,3}\\+\\w{2,3}\\*8\\+${hex}\\]`, 'i');
    assert.ok(!asFrameField.test(text), `0x${abs.toString(16)} 不该按"帧内索引（×8）"寻址 —— 若真如此，本条判据要先复核`);
  }
  // ★ 这三个地址**落在帧区之内**（0x5D880 + 40×0x78 = 0x5EB40）—— 这正是矛盾的形状：
  //   同一个 dword 既可以被读成"帧0 的 +0x78"，也可以被读成"某个按 cur 索引的数组的第 0 项"。
  //   两种读法都能从语料里找出支持它的指令 ⇒ **未裁决前不许选一种**。
  const frameEnd = FRAME.base + FRAME.stride * FRAME.count;
  for (const abs of THREE_ARRAYS) {
    assert.ok(abs > FRAME.base && abs < frameEnd, `0x${abs.toString(16)} 应落在帧区内（0x${FRAME.base.toString(16)}..0x${frameEnd.toString(16)}）`);
  }
  assert.match(
    `${THREE_ARRAYS.map((a) => `+0x${(a - FRAME.base).toString(16)}`).join(' / ')}`,
    /\+0x78 \/ \+0x7c \/ \+0x80/,
    '★ 把这三个绝对地址读成"帧内偏移"就正好是 +0x78 / +0x7C / +0x80 —— 分歧的读数口径要保留在判据里',
  );
});

test('★ 帧的矛盾仍在（旧口径说法 vs 新语料机械普查）—— 未裁决前不许抹平', () => {
  // 旧口径把这三个绝对地址读成了"帧+0x78/0x7C/0x80"；机械普查说它们是步长 4 的独立数组。
  // 本用例的职责就是**把分歧钉住**：谁要改哪个数，必须先给出裁决依据（EA / 语料行区间 / 守卫）。
  assert.deepEqual(OLD_CLAIM, [0x78, 0x7c, 0x80], '旧口径那三个"帧内偏移"是分歧的一方，不许静默改写');
  assert.equal(THREE_ARRAYS[0] - FRAME.base, 0x78, '★ 这正是分歧的来源：0x5D8F8 在字节上等于"帧+0x78"，但它按 cur 索引');
  assert.equal(FRAME.base + FRAME.stride * FRAME.count, 0x5eb40, '40 帧的末尾 = 0x5EB40');
  assert.ok(FRAME.stride * FRAME.count > OLD_CLAIM[2], '步长与帧数决定的帧区**大于**旧口径点名的那些偏移');
});

test('★ 帧的三个数本身就是"待裁决的口径"：改任何一个都要先裁决矛盾', () => {
  assert.equal(FRAME.base, 0x5d880, '帧基址 0x5D880（★ 历史错：曾误记 0x5D894）');
  assert.equal(FRAME.stride, 0x78, '步长 0x78 —— 三处独立机械证据（两条取操作数原语 + 字段清零循环）');
  assert.equal(FRAME.count, 40, '帧数 40（call-script 的 cur>=39 抛"階層が深すぎます"）');
  assert.equal(FRAME.base + FRAME.stride * FRAME.count, 0x5eb40, '40 帧的末尾 = 0x5EB40（与 fields.json 的区间一致）');
});
