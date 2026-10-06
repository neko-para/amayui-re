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
