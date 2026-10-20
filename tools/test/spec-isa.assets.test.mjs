/** @env assets @kind gate @why 协议章的结构性断言与真实语料脱节（头布局 / opcode 域 / 表区越界 / 指令切分），读者会按错的字节结构写解码器 */
/**
 * tools/test/spec-isa.assets.test.mjs —— **CORE-ISA 的语料侧守卫**
 *
 * 它把 `data/spec/chapters/10-format.md` 的 CORE-ISA 条目落到真实脚本上：
 * ① 签名与版本串（4 + 4 字节）；② 头长 = 32 + 子头长度；③ 三组表偏移是 body 内 dword 下标且首尾相接；
 * ④ opcode 域 `[0, 0x400)`；⑤ 直线解码不会越过表区起点；⑥ 指令切分用 `4 + 8×argc`。
 *
 * ★ 它**不**证明"与引擎等价"：只证明"按本章的结构解码，真实语料不会卡住或越界"。
 * ★ 已知 2 个脚本（CGVIEWER / CTINIT）的直线解码会走进数据区 —— 这是 `CORE-ISA-EXEC-END` 的观测证据，
 *   不是本守卫的失败；因此本守卫不假设"直线解码一定停在代码末尾"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { loadManifest } from '../lib/manifest.mjs';

const TABLE_PATH = path.join(REPO_ROOT, 'data/spec/format/opcode-table.json');
const u32 = (buf, off) => (off + 4 <= buf.length ? buf.readUInt32LE(off) : null);

/** 收集脚本：安装根目录的 *.BIN + `_extracted` 下的 *.BIN（后者缺席就跳过） */
function collectScripts(root) {
  const top = [];
  const nested = [];
  for (const name of fs.readdirSync(root)) {
    if (/\.bin$/i.test(name)) top.push(path.join(root, name));
  }
  const ext = path.join(root, '_extracted');
  if (fs.existsSync(ext)) {
    const walk = (dir) => {
      for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, ent.name);
        if (ent.isDirectory()) walk(p);
        else if (/\.bin$/i.test(ent.name)) nested.push(p);
      }
    };
    walk(ext);
  }
  return { top, nested };
}

test('★ 语料侧：签名 / 头长 / 表偏移 / opcode 域 / 表区不越界', async (t) => {
  const { roots } = loadManifest();
  const gameInstall = roots?.gameInstall;
  if (!gameInstall || !fs.existsSync(gameInstall)) {
    t.skip('gameInstall 不在本机（corpus/assets.json 的 roots）');
    return;
  }

  const table = new Map();
  for (const row of JSON.parse(fs.readFileSync(TABLE_PATH, 'utf8'))) {
    assert.ok(Number.isInteger(row.opcode) && row.opcode >= 0 && row.opcode < 0x400, `规范表 opcode 越域：${row.opcode}`);
    table.set(row.opcode, row.argc);
  }

  const { top, nested } = collectScripts(gameInstall);
  const all = [...top, ...nested];
  assert.ok(all.length >= 400, `脚本数太少（${all.length}）—— 语料没到位？`);

  let scripts = 0;
  let sys3 = 0;
  let headerWrong = 0;
  let tableBroken = 0;
  let tableOverrun = 0;
  let unknownOpcode = 0;
  let overflow = 0;
  let table3EndMismatchTop = 0;

  for (const file of all) {
    const buf = fs.readFileSync(file);
    const sig = buf.subarray(0, 4).toString('latin1');
    if (sig !== 'SYS4' && sig !== 'SYS3') continue; // 非脚本（userdata / 容器）
    scripts += 1;
    if (sig === 'SYS3') { sys3 += 1; continue; }

    // 版本串：4 字节可打印 ASCII
    const ver = buf.subarray(4, 8).toString('latin1');
    if (!/^[\x20-\x7e]{4}$/.test(ver)) { headerWrong += 1; continue; }

    // 头长 = 32 + 子头长度
    const subLen = u32(buf, 32);
    const headerLen = 32 + subLen;
    if (subLen === null || headerLen < 32 || headerLen > buf.length) { headerWrong += 1; continue; }
    if (headerLen !== 0x3c) headerWrong += 1; // 本作语料：子头 28 ⇒ 头长 0x3C

    const body = buf.subarray(headerLen);
    const len1 = u32(buf, 36); const off1 = u32(buf, 40);
    const len2 = u32(buf, 44); const off2 = u32(buf, 48);
    const len3 = u32(buf, 52); const off3 = u32(buf, 56);
    const fieldsOk = [len1, off1, len2, off2, len3, off3].every((x) => Number.isInteger(x));
    if (!fieldsOk) { headerWrong += 1; continue; }

    // 三组偏移是 body 内 dword 下标；三表首尾相接
    const contiguous = off2 === off1 + len1 && off3 === off2 + len2;
    if (!contiguous) tableBroken += 1;
    const tableBytes = off1 * 4;
    if (tableBytes > body.length) tableBroken += 1;
    // gameInstall 顶层：第 3 组正好结束于 body 末尾（`_extracted` 是另一份语料，不参与）
    if (top.includes(file) && off3 * 4 + len3 * 4 !== body.length) table3EndMismatchTop += 1;

    // 直线解码：按 4 + 8×argc 推进；opcode 不在表里 / 越过表区起点就停（走进数据区是预期的）
    let ip = 0;
    for (let guard = 0; guard < 5_000_000; guard += 1) {
      if (ip + 4 > body.length) break;
      if (body.length >= tableBytes && ip > tableBytes) { tableOverrun += 1; break; }
      const opcode = body.readUInt32LE(ip);
      const argc = table.get(opcode);
      if (argc === undefined) { unknownOpcode += 1; break; }
      ip += 4 + 8 * argc;
      if (ip > body.length) { overflow += 1; break; }
    }
  }

  assert.ok(scripts >= 400, `脚本太少（${scripts}）`);
  assert.equal(sys3, 0, '本作语料不应出现 SYS3 签名');
  assert.equal(headerWrong, 0, `头布局断言失败 ${headerWrong} 个（期望 4B 签名 + 4B 版本串 + 6×u32 + 子头，头长 0x3C）`);
  assert.equal(tableBroken, 0, `表偏移断言失败 ${tableBroken} 个（三组 (len,off) 必须首尾相接且落在 body 内）`);
  assert.equal(tableOverrun, 0, `直线解码越过表区起点 ${tableOverrun} 个（代码区应在表区之前）`);
  assert.equal(table3EndMismatchTop, 0, `gameInstall 顶层：第 3 张表结束 != body 末尾 ${table3EndMismatchTop} 个`);

  // 分派器的 opcode 域：清单里 0x411A5D 必须是符号检查（js），0x411A5F 必须与 400h 比较
  const lst = path.join(REPO_ROOT, 'corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst');
  if (fs.existsSync(lst)) {
    const lines = fs.readFileSync(lst, 'utf8').split('\n');
    const at = (ea) => lines.find((l) => l.startsWith(`.text:${ea} `));
    const neg = at('00411A5D');
    const cmp = at('00411A5F');
    assert.ok(neg && cmp, '语料里找不到 0x411A5D / 0x411A5F（反汇编语料过期？）');
    assert.match(neg, /\bjs\b/i, '0x411A5D 不是符号检查 ⇒ opcode 域断言与语料不符');
    assert.match(cmp, /400h/i, '0x411A5F 不是与 400h 比较 ⇒ opcode 域断言与语料不符');
  } else {
    t.diagnostic('反汇编语料缺席：跳过 opcode 域常量的核对');
  }
  // 已知 2 个脚本会走进数据；不把"走进数据"当失败，但要可见
  t.diagnostic(`脚本 ${scripts} · 表尾不匹配(_extracted 不计) ${table3EndMismatchTop} · 停在未知 opcode ${unknownOpcode} · 越过 body ${overflow}`);
});
