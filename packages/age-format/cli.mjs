#!/usr/bin/env node
/**
 * packages/age-format/cli.mjs —— **CLI**：ALF / AGF / ASM 三套容器的查看、解包、重打包与**往返复验**
 *
 * 分层（`tools/README.md` §0 的分层约定同样适用）：
 *   · 领域模型在 `src/`（`alf.mjs` / `agf.mjs` / `asm/`）与纯工具 `src/lzss.mjs`；
 *   · 本文件只做"参数 → 模型 → 输出"，**不实现任何格式规则**。
 *
 * ★ 判据的**一条命令版**：`node packages/age-format/cli.mjs verify`
 *   —— 对清单登记的样本跑「解包 → 重打包逐字节相同」；样本不在场就**跳过**并报数。
 * ★ 不捕获子进程输出（受限沙箱里 `stdio:'pipe'` 会 EPERM）：这里也不 spawn 任何东西。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { readAlf, writeIndex, writeArchive, loadPayloads, unpackTo, declaredPayloadBytes } from './src/alf.mts';
import * as lzss from './src/lzss.mts';
import { readAgf, writeAgf, roundTripEqual, decodeRgba } from './src/agf.mts';
import { disassemble, assemble, bytesEqual } from './src/asm/index.mts';
import { loadSample, fileOf, sha256 } from './test/samples.mjs';

const HELP = `packages/age-format/cli.mjs —— AGE 容器（ALF / AGF / ASM）

  verify                                   ★ 对清单样本跑「解包 → 重打包逐字节相同」（样本缺席则跳过）
  alf-list   <索引文件>                     列出 ALF 目录：归档数 / 条目数 / 声明载荷量
  alf-unpack <索引文件> --out <目录>         解包到 <目录>/<归档名前缀>/<条目名>
  alf-repack <索引文件> --out <文件>         重打包索引（未改动时应与原文件逐字节相同）
  agf-info   <*.AGF>                       头部三段尺寸 + 尺寸/bpp/调色板项数 + 是否 ACIF
  asm-dis    <*.BIN> --out <*.txt>          反汇编（UTF-8 文本）
  asm-asm    <*.txt> --out <*.BIN>          重汇编
`;

function parseArgs(argv) {
  const out = { cmd: null, rest: [], flags: {} };
  const takesValue = new Set(['out', 'id']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--help' || a === '-h') {
      out.cmd = 'help';
    } else if (a.startsWith('--')) {
      const k = a.slice(2);
      if (!takesValue.has(k)) throw new Error(`不认识的选项：${a}`);
      out.flags[k] = argv[i + 1];
      i += 1;
    } else if (out.cmd === null) {
      out.cmd = a;
    } else {
      out.rest.push(a);
    }
  }
  return out;
}

const need = (cond, msg) => {
  if (!cond) throw new Error(msg);
};

/** 索引 + 数据体：ALF 的归档按"索引所在目录"解析（与格式本身一致） */
function openAlf(file) {
  need(file && fs.existsSync(file), `索引文件不存在：${file}`);
  return readAlf(file);
}

function cmdVerify() {
  const lines = [];
  let checked = 0;
  let skipped = 0;
  let failed = 0;

  const alf = loadSample('assets/samples-alf');
  if (!alf) {
    skipped += 3;
    lines.push('… ALF 样本不在场（原始游戏文件不入库）—— 跳过 3 项');
  } else {
    const idx = fileOf(alf, 'APPEND02.AAI');
    const arc = fileOf(alf, 'APPEND02.ALF');
    const a = readAlf(idx.abs);
    a.archivePath = (name) => (name === 'APPEND02.ALF' ? arc.abs : path.join(a.baseDir, name));
    const okIndex = writeIndex(a).equals(idx.buf);
    lines.push(`${okIndex ? '✔' : '✖'} ALF 索引重打包逐字节相同（${idx.buf.length} B${okIndex ? '' : ' ← 不一致'}）`);
    checked += 1;
    failed += okIndex ? 0 : 1;

    const r = loadPayloads(a);
    const arch = writeArchive(a, 0);
    const okArc = arch.gaps.length === 0 && arch.data.equals(arc.buf);
    lines.push(
      `${okArc ? '✔' : '✖'} ALF 数据体重打包逐字节相同（${arc.buf.length} B，载荷 ${r.loaded} 件` +
        `${arch.gaps.length ? `，空洞 ${arch.gaps.length} 处` : ''}${okArc ? '' : ' ← 不一致'}）`,
    );
    checked += 1;
    failed += okArc ? 0 : 1;

    const toc = a.section.data.subarray(0, a.section.size);
    const okLzss = (() => {
      const { pack, unpack } = lzss;
      const re = pack(toc);
      return re.equals(a.section.packed) && unpack(re, re.length, a.section.size).data.subarray(0, a.section.size).equals(toc);
    })();
    lines.push(`${okLzss ? '✔' : '✖'} ALF 目录区 LZSS 解压↔重压逐字节相同`);
    checked += 1;
    failed += okLzss ? 0 : 1;
  }

  const agf = loadSample('assets/samples-agf');
  if (!agf) {
    skipped += 3;
    lines.push('… AGF 样本不在场 —— 跳过 3 项');
  } else {
    for (const name of ['MI042.AGF', 'MI040.AGF', 'SO002.AGF']) {
      const f = fileOf(agf, name);
      const a = readAgf(f.abs);
      const ok = roundTripEqual(a) && sha256(writeAgf(a)) === f.sha256;
      lines.push(
        `${ok ? '✔' : '✖'} AGF ${name} 逐字节相同（${a.width}x${a.height}@${a.bpp}，` +
          `meta ${a.metaUnpackedSize}/${a.metaPackedSize}，body ${a.bodyUnpackedSize}/${a.bodyPackedSize}）`,
      );
      checked += 1;
      failed += ok ? 0 : 1;
    }
  }

  const asm = loadSample('assets/samples-asm');
  if (!asm) {
    skipped += 1;
    lines.push('… ASM 样本不在场 —— 跳过 1 项');
  } else {
    let ok = true;
    const detail = [];
    for (const e of asm.entries) {
      const back = assemble(disassemble(e.buf));
      // ★ 用包自己的 `bytesEqual`：`assemble()` 返回 `Uint8Array`（不许绑 Node）⇒ 没有 `.equals`
      const eq = bytesEqual(back, e.buf);
      if (!eq) ok = false;
      detail.push(`${path.basename(e.rel)}${eq ? '✔' : '✖'}`);
    }
    lines.push(`${ok ? '✔' : '✖'} ASM 四个样本 反汇编 → 重汇编逐字节相同（${detail.join(' ')}）`);
    checked += 1;
    failed += ok ? 0 : 1;
  }

  return { lines, checked, skipped, failed };
}

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.cmd === null || args.cmd === 'help') {
    process.stdout.write(HELP);
    return 0;
  }

  if (args.cmd === 'verify') {
    const { lines, checked, skipped, failed } = cmdVerify();
    for (const l of lines) process.stdout.write(`${l}\n`);
    process.stdout.write(`\n共核对 ${checked} 项，跳过 ${skipped} 项，失败 ${failed} 项。\n`);
    if (skipped > 0) {
      process.stdout.write('（跳过 = 样本不在场。样本是原始游戏文件、按口径不入库，见 corpus/assets/samples.md）\n');
    }
    return failed > 0 ? 1 : 0;
  }

  if (args.cmd === 'alf-list') {
    const a = openAlf(args.rest[0]);
    process.stdout.write(`索引      ${a.file}\n`);
    process.stdout.write(`布局      ${a.layout.name}（${a.layout.signature}，段起点 ${a.layout.sectionPos}）\n`);
    process.stdout.write(`目录区    ${a.section.length} B 压缩 → ${a.section.size} B 解压\n`);
    process.stdout.write(`归档 ${a.archiveCount} 个：${a.archives.map((x) => x.filename).join(', ')}\n`);
    process.stdout.write(`条目 ${a.fileCount} 个，声明载荷 ${(declaredPayloadBytes(a) / 1048576).toFixed(1)} MB\n`);
    const flagged = a.entries.filter((e) => !a.archives[e.archiveIndex]);
    if (flagged.length) process.stdout.write(`⚠ ${flagged.length} 个条目的 archiveIndex 越界\n`);
    return 0;
  }

  if (args.cmd === 'alf-unpack') {
    const a = openAlf(args.rest[0]);
    const out = args.flags.out;
    need(out, '需要 --out <目录>');
    const r = loadPayloads(a);
    const w = unpackTo(a, out);
    process.stdout.write(`解包 ${w.written.length} 件 → ${out}\n`);
    if (r.missingArchives.length) process.stdout.write(`⚠ 缺席归档：${r.missingArchives.join(', ')}\n`);
    return 0;
  }

  if (args.cmd === 'alf-repack') {
    const a = openAlf(args.rest[0]);
    const out = args.flags.out;
    need(out, '需要 --out <文件>');
    const buf = writeIndex(a);
    fs.writeFileSync(out, buf);
    const same = a.buf.length === buf.length && buf.equals(a.buf);
    process.stdout.write(`写出 ${out}（${buf.length} B）${same ? '—— 与原文件逐字节相同' : '—— ⚠ 与原文件不同（说明目录被改过）'}\n`);
    return 0;
  }

  if (args.cmd === 'agf-info') {
    const f = args.rest[0];
    need(f && fs.existsSync(f), `文件不存在：${f}`);
    const a = readAgf(f);
    process.stdout.write(`文件      ${f}（${a.buf.length} B，version ${a.version}）\n`);
    process.stdout.write(`meta      ${a.metaUnpackedSize} → ${a.metaPackedSize} B${a.metaUnpackedSize === a.metaPackedSize ? '（原样）' : '（压缩）'}\n`);
    process.stdout.write(`body      ${a.bodyUnpackedSize} → ${a.bodyPackedSize} B${a.bodyUnpackedSize === a.bodyPackedSize ? '（原样）' : '（压缩）'}\n`);
    process.stdout.write(`ACIF      ${a.acif ? `有（alpha ${a.acif.alphaUnpackedSize} → ${a.acif.alphaPackedSize} B）` : '无'}\n`);
    process.stdout.write(`像素      ${a.width}x${a.height}@${a.bpp}\n`);
    const img = decodeRgba(a);
    process.stdout.write(`解码      ${img.rgba.length} 字节 RGBA（top-down）\n`);
    return 0;
  }

  if (args.cmd === 'asm-dis' || args.cmd === 'asm-asm') {
    const f = args.rest[0];
    const out = args.flags.out;
    need(f && fs.existsSync(f), `文件不存在：${f}`);
    need(out, '需要 --out <文件>');
    if (args.cmd === 'asm-dis') {
      const text = disassemble(fs.readFileSync(f));
      fs.writeFileSync(out, text, 'utf8');
      process.stdout.write(`反汇编 ${f} → ${out}（${text.split('\n').length} 行）\n`);
    } else {
      const text = fs.readFileSync(f, 'utf8');
      const bin = assemble(text);
      fs.writeFileSync(out, bin);
      process.stdout.write(`重汇编 ${f} → ${out}（${bin.length} B）\n`);
    }
    return 0;
  }

  process.stderr.write(`未知动作：${args.cmd}\n${HELP}`);
  return 2;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.message}\n`);
    process.exitCode = 2;
  }
}
