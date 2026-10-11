/** @env external @kind gate @why EA→文件偏移 的 VA/RVA 口径又错了（对真实镜像会恒返回 null，锚点全落不了） */
/**
 * tools/test/ledger.pe.external.test.mjs —— **`peOffsetOf` 的真实镜像回归**（要旧仓里的 PE）
 *
 * ★ 为什么单独一份、且用真镜像：`peOffsetOf` 第一版把锚点里的 **EA（VA）** 直接和节表的
 *   `VirtualAddress`（**RVA**）比 ⇒ 对**任何真实镜像恒 null**，报出"EA 不在任何 PE 节里（或不是 PE）"。
 *   那句话**看起来像正常的地址错误**，实际是口径错。
 *   `tools/test/ledger.test.mjs` 里的合成 PE 的 ImageBase 是 0（VA ≡ RVA）⇒ **测不到这一半**。
 *
 * 判据（红得有意义）：同一份真镜像上
 *   * `peOffsetOf(buf, ImageBase + 0x1000) === 0x400`（`.text` 的 VA → 文件偏移）
 *   * 旧口径（把 VA 当 RVA）会 null ⇒ 这两条**只有口径对才绿**
 *   * ★ **段内**函数起点（`0x497620` / `0x4976A0`）：偏移处字节必须逐字等于 `.lst` 里那几条指令的编码
 *     —— 只钉节起点测不到"段内整体少一页 `0x1000`"这类错位（它会静默落进前一个函数的体内）
 *
 * 运行：`pnpm test:all`（旧仓不在本机时如实 skip）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { peImageBase, peMap, peOffsetOf } from '../lib/ledger.mjs';

const MANIFEST = path.join(REPO_ROOT, 'corpus', 'assets.json');

/** 按清单条目解析出真镜像的绝对路径（不硬编码旧仓路径） */
function imageOf(id) {
  const m = JSON.parse(fs.readFileSync(MANIFEST, 'utf8'));
  const e = m.entries.find((x) => x.id === id);
  if (!e) return null;
  const o = (e.origin ?? [])[0];
  if (!o) return null;
  const root = m.roots?.[o.root];
  if (typeof root !== 'string') return null;
  const abs = path.join(root, o.path);
  return fs.existsSync(abs) ? abs : null;
}

for (const id of ['binary/age-sectfix', 'binary/age-original']) {
  test(`★ ${id}：EA（VA）→ 文件偏移，口径必须是 VA − ImageBase`, (t) => {
    const abs = imageOf(id);
    if (!abs) {
      t.skip(`真镜像不在场（${id} 的 origin 根没配 / 文件缺失）`);
      return;
    }
    const buf = fs.readFileSync(abs);
    const peAt = buf.readUInt32LE(0x3c);
    const imageBase = peImageBase(buf, peAt);
    assert.ok(imageBase > 0, `真镜像的 ImageBase 必须 > 0（实测 0x400000），实际 0x${imageBase.toString(16)}`);

    // 头部注释里写的就是这两个数（实测两份镜像都是 .text va=0x1000 / rawptr=0x400）
    const atTextStart = peOffsetOf(buf, imageBase + 0x1000);
    assert.equal(atTextStart, 0x400, `ImageBase+0x1000 应映射到文件偏移 0x400，实际 ${atTextStart}`);

    // ★ 反例：把 VA 当 RVA（旧口径）必须**映射不到** —— 这条让"口径回退"当场红
    assert.equal(peOffsetOf(buf, 0x1000), null, '0x1000（裸 RVA）不该被当成 VA 命中 —— 除非 ImageBase 真的是 0');

    // 诊断信息里要能看出用了哪个口径与哪一节
    const m = peMap(buf, imageBase + 0x1000);
    assert.equal(m.rva, 0x1000, 'rva 应是 0x1000');
    // ★ 只要求"名字不与 .text 冲突"：实测 `binary/age-original` 那份镜像的节名是**空串**（节表无名字），
    //   那是**镜像属性**而不是口径错 ⇒ 守卫不许把合法的空名判红。
    assert.ok(m.section === '' || /^\.text/.test(m.section), `应落在首个代码节，实际 ${JSON.stringify(m.section)}`);
    assert.equal(m.offset, 0x400);

    // 段内偏移：EA 每 +1，文件偏移也 +1
    assert.equal(peOffsetOf(buf, imageBase + 0x1010), 0x410);
  });
}

/**
 * ★★ **段内** EA 的已知答案回归 —— 上面的用例只钉住 `.text` 的**第一条指令**（`ImageBase + 0x1000`）。
 *
 * ## 为什么必须补这一条（实测发生过一次误判）
 * 只钉节起点，测不到"段内更深的 EA 整体错一格"这一类：错位后它落进**前一个函数的体内**，
 * 而"读到了字节"看起来完全正常。实测症状：一条台账 claim 说"`0x497620` 被映到 `0x95a20`（EA 少一页 `0x1000` 的那个数）"，
 * 而 `0x95a20` 那 13 字节是 `6b 83 bd b0 …`、`0x497620` 处才是 `55 8b ec 56 …`。
 * ★ 事后复核（台账 `tool/ledger/pe-offset-and-anchor-alignment`）判明：**映射没错，差的是两个 EA** ——
 *   `peOffsetOf(buf, 0x497620)` = `0x96a20`，`0x95a20` 属 `0x496620`（= 少一页的那个 EA，且它**不是**指令起点）。
 *   ⇒ 这里同时钉两件事：① 段内函数起点的**已知答案**（字节逐字对上 `.lst`）；
 *   ② 一条**真·指令起点**的负例（`0x49661F`）+ 一条**只验算术**的负例（`0x496620`，注明它不是起点）。
 *
 * ## 期望值从哪来（★ 手写自 `.lst` 的助记符，不是抄 actual）
 * * `0x497620`（`sub_497620 proc near`，`.lst:240025-240029`）：
 *   `push ebp`=55 · `mov ebp, esp`=8B EC · `push esi`=56 · `mov esi, ecx`=8B F1 ·
 *   `cmp dword ptr [esi+420h], 0`=83 BE 20 04 00 00 00
 * * `0x4976A0`（`sub_4976A0 proc near`，`.lst:240099-240105`）：
 *   `push ebp` · `mov ebp, esp` · `sub esp, 8`=83 EC 08 · `mov eax, ds:dword_55E1BC`=A1 BC E1 55 00 ·
 *   `push esi` · `mov esi, ecx` · `mov ecx, [eax+5D880h]`=8B 88 80 D8 05 00
 */
const KNOWN_SITES = [
  { ea: 0x497620, hex: '558bec568bf183be2004000000', from: '.lst:240025-240029' },
  { ea: 0x4976a0, hex: '558bec83ec08a1bce15500568bf18b8880d80500', from: '.lst:240099-240105' },
];

test('★ 段内函数起点：偏移处字节必须逐字等于 `.lst` 里那几条指令的编码（少一页 0x1000 ⇒ 落进前一个函数体内）', (t) => {
  const abs = imageOf('binary/age-sectfix');
  if (!abs) {
    t.skip(`真镜像不在场（binary/age-sectfix 的 origin 根没配 / 文件缺失）`);
    return;
  }
  const buf = fs.readFileSync(abs);
  for (const s of KNOWN_SITES) {
    const off = peOffsetOf(buf, s.ea);
    assert.notEqual(off, null, `EA 0x${s.ea.toString(16)} 必须映射得到（它在 .text 里）`);
    const got = buf.subarray(off, off + s.hex.length / 2).toString('hex');
    assert.equal(got, s.hex, `EA 0x${s.ea.toString(16)} → 偏移 0x${off.toString(16)} 处的字节应是 ${s.hex}（手写自 ${s.from}）`);
  }
  // ★ 负例（把"那个数"钉在案上）：`0x95a20` 是 **EA `0x496620`** 的偏移 —— 而 `0x496620`
  //   **不是**任何站点的起点：本语料里**没有**一条 `.lst` 行以 `.text:00496620` 开头（实测命中 0 行），
  //   它是 `0x49661F jz short loc_49668C`（2 字节 `74 6B`）的 **rel8 操作数字节**。
  //   ⇒ 下面这条断言**只验 EA → 偏移的算术**（VA − ImageBase 线性、两个不同 EA 不许映到同一处），
  //   它**不**声称 `0x496620` 曾是函数 / 指令起点。
  //   ★ 顺带纠一处误引（台账 `tool/ledger/pe-offset-and-anchor-alignment` 已证伪）：**不是**"工具把
  //   `0x497620` 打成了 `0x95a20`" —— `peOffsetOf(buf, 0x497620)` = `0x96a20`，`0x95a20` 属 `0x496620`。
  assert.equal(peOffsetOf(buf, 0x496620), 0x95a20, 'EA 0x496620（= 0x497620 − 0x1000）映到 0x95a20（★ 只验算术）');
  assert.notEqual(peOffsetOf(buf, 0x497620), peOffsetOf(buf, 0x496620), '0x497620 与 0x496620 是两个 EA ⇒ 偏移必须不同');
  // ★ 真·**指令起点**的负例（与上面那条不同：这条的 EA 有 `.lst` 行首作证）：`0x49661F` = `jz short loc_49668C`
  //   ⇒ 偏移处字节必须逐字等于 `74 6B`（期望值手写自助记符：`jz short` = `74`、rel8 = 0x49668C − 0x496621 = 0x6B）
  {
    const off = peOffsetOf(buf, 0x49661f);
    assert.equal(off, 0x95a1f, 'EA 0x49661F（`.lst` 行首）→ 偏移 0x95a1F');
    assert.equal(
      buf.subarray(off, off + 2).toString('hex'),
      '746b',
      'EA 0x49661F 处必须逐字是 `jz short loc_49668C` 的编码 74 6B（手写自 `.lst`）',
    );
  }
});

test('★ 合成 PE（ImageBase=0）仍然按"给的就是 RVA"兜底 —— 两种口径都要能用', () => {
  const pe = Buffer.alloc(0x600);
  pe.write('MZ', 0, 'latin1');
  pe.writeUInt32LE(0x80, 0x3c);
  pe.write('PE\0\0', 0x80, 'latin1');
  pe.writeUInt16LE(1, 0x80 + 6);
  pe.writeUInt16LE(0, 0x80 + 20); // 无可选头 ⇒ magic 读出来是 0 ⇒ ImageBase 视为 0
  const s = 0x80 + 24;
  pe.write('.text\0\0\0', s, 'latin1');
  pe.writeUInt32LE(0x1000, s + 12);
  pe.writeUInt32LE(0x200, s + 16);
  pe.writeUInt32LE(0x400, s + 20);
  assert.equal(peImageBase(pe, 0x80), 0);
  assert.equal(peOffsetOf(pe, 0x1000), 0x400, 'ImageBase=0 ⇒ VA≡RVA，0x1000 直接映射');
  assert.equal(peOffsetOf(pe, 0x1010), 0x410);
  assert.equal(peOffsetOf(pe, 0x2000), null, '不在任何节里 ⇒ null（不许瞎猜）');
});
