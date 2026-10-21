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
import { extractHandlers, listingPath } from '../lib/opcodes.mjs';

const TABLE_PATH = path.join(REPO_ROOT, 'data/spec/format/opcode-table.json');
const u32 = (buf, off) => (off + 4 <= buf.length ? buf.readUInt32LE(off) : null);

/**
 * 收集脚本：**只收安装根目录的 `*.BIN`**。
 * ⛔ 不收 `_extracted/`：那是历史汉化版本在旧仓里留下的临时解压（overlay 产物），
 * 不是安装数据；把它算进语料会让结构断言量到另一份文件（见 `docs/01-translation/patch-design.md`）。
 */
function collectScripts(root) {
  const top = [];
  for (const name of fs.readdirSync(root)) {
    if (/\.bin$/i.test(name)) top.push(path.join(root, name));
  }
  return top;
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

  const all = collectScripts(gameInstall);
  // 下限只防"目录空了 / 指错根"；真实安装的顶层 `.BIN` 是三位数（本机 105），不写死具体数
  assert.ok(all.length >= 50, `脚本数太少（${all.length}）—— 安装根不对，或顶层没有散装脚本？`);

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
    // 安装根顶层：第 3 组正好结束于 body 末尾
    if (off3 * 4 + len3 * 4 !== body.length) table3EndMismatchTop += 1;

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

  assert.ok(scripts >= 50, `脚本太少（${scripts}）`);
  assert.equal(sys3, 0, '本作语料不应出现 SYS3 签名');
  assert.equal(headerWrong, 0, `头布局断言失败 ${headerWrong} 个（期望 4B 签名 + 4B 版本串 + 6×u32 + 子头，头长 0x3C）`);
  assert.equal(tableBroken, 0, `表偏移断言失败 ${tableBroken} 个（三组 (len,off) 必须首尾相接且落在 body 内）`);
  assert.equal(tableOverrun, 0, `直线解码越过表区起点 ${tableOverrun} 个（代码区应在表区之前）`);
  assert.equal(table3EndMismatchTop, 0, `第 3 张表结束 != body 末尾 ${table3EndMismatchTop} 个`);

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
  t.diagnostic(`脚本 ${scripts} · 表尾不匹配 ${table3EndMismatchTop} · 停在未知 opcode ${unknownOpcode} · 越过 body ${overflow}`);
});


/**
 * 指令表对账（`REQ-01M5ED2AD7SKV077BRVRVGNE3R7`）：
 * ① `hasHandler` 必须与语料分派表的**赋值行**逐条一致 —— 否则分发物判不出"哪 30 条执行即抛"；
 * ② 每个 handler 里能**直接读出**的长度槽立即数（`mov dword ptr [esi+ecx*8+5D8F4h], imm`）必须等于 `2×argc+1`。
 * ★ ②只覆盖"恰好写一条立即数"的 handler（约 446 / 544）；其余 handler 走间接写入或多次写入，
 *   不在这一条守卫的范围（它们由各自的 opcode 规则覆盖）。
 */
test('★ 指令表对账：hasHandler 与语料分派表一致；能直接读出的长度槽 = 2×argc+1', (t) => {
  const lst = listingPath(REPO_ROOT);
  if (!lst) {
    t.skip('反汇编语料不在场（corpus/disasm/files/*.lst）');
    return;
  }
  const text = fs.readFileSync(lst, 'utf8');
  const { byOpcode, problems } = extractHandlers(text);
  assert.deepEqual(problems, [], '分派表提取本身有异常（同一 opcode 两条 handler / 偏移对不上）');

  const rows = JSON.parse(fs.readFileSync(TABLE_PATH, 'utf8'));
  const wrongHas = rows.filter((r) => r.hasHandler !== byOpcode.has(r.opcode));
  assert.deepEqual(wrongHas.map((r) => r.opcode), [],
    'hasHandler 与语料分派表不一致（true = 有非缺省处理函数）');
  assert.equal(rows.filter((r) => r.hasHandler).length, byOpcode.size,
    '有处理函数的条目数必须等于语料里的赋值行数');
  assert.equal(rows.filter((r) => !r.hasHandler).length, rows.length - byOpcode.size,
    '缺省条目数 = 表大小 − 赋值行数');

  // 每个函数区间内"恰好一条"直接写长度槽的立即数
  const wrote = new Map();
  let current = null;
  for (const line of text.split('\n')) {
    const proc = /^\s*[\w.]+:([0-9A-Fa-f]{8})\s+(\S+)\s+proc near/.exec(line);
    if (proc) { current = proc[2]; continue; }
    const m = /mov\s+dword ptr \[esi\+ecx\*8\+5D8F4h\],\s*([0-9A-Fa-fx]+h?)\b/.exec(line);
    if (!m || !current) continue;
    const raw = m[1];
    const value = raw.endsWith('h') ? Number.parseInt(raw.slice(0, -1), 16) : Number.parseInt(raw, 10);
    if (!wrote.has(current)) wrote.set(current, []);
    wrote.get(current).push(value);
  }
  const direct = [];
  const mismatched = [];
  for (const [opcode, handler] of byOpcode) {
    const values = wrote.get(handler);
    if (!values || values.length !== 1) continue;
    const row = rows.find((r) => r.opcode === opcode);
    if (!row) continue;
    direct.push(opcode);
    if (values[0] !== 2 * row.argc + 1) {
      mismatched.push(`0x${opcode.toString(16)}: argc=${row.argc} 期望 ${2 * row.argc + 1}，handler 写 ${values[0]}`);
    }
  }
  // 防"守卫静默缩小"：直接可读的 handler 必须占多数（实测 446 / 544）
  assert.ok(direct.length >= 400, `直接可读的长度槽 handler 太少（${direct.length}）—— 提取口径漂了？`);
  assert.deepEqual(mismatched, [], 'handler 写的长度槽与指令表的 argc 不一致');
  t.diagnostic(`对账：${rows.length} 条表项 · ${byOpcode.size} 条有 handler · ${rows.length - byOpcode.size} 条缺省 · ${direct.length} 条直接长度槽全一致`);
});
