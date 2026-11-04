#!/usr/bin/env node
/**
 * tools/lift-cp932-gen.mjs — 生成 `apps/emulator-lift/src/runtime/cp932_table.h`（CP932 表）
 *
 * 为什么自带表（plan §7.1 的两个选项之一）：
 *   · **确定性**：本线要求"同一份存档名在 Windows / macOS 上编出同一串字节"（plan §7.1 末段）。
 *     用宿主 `MultiByteToWideChar` 会让两个平台各有一套表；libiconv 又会引入 LGPL 静态链接义务
 *     + 一个 vcpkg port ⇒ 表钉死成**入库的纯文本**，三平台逐字节一致，也不要新依赖。
 *   · **口径**：双字节部分与 WHATWG `shift_jis` 逐对一致（`tools/lib/cp932.mjs` 头注释：
 *     9604 个已定义 pair 全比过、0 差异）；单字节只有 8 个差异（0x1A/0x1C/0x7F/0x80/0xA0/0xFD/0xFE/0xFF），
 *     按下表逐条修正 —— 与 `tools/lib/cp932.mjs` 的 `CP932_SINGLE` **同一份口径**。
 *
 * 用法：`node tools/lift-cp932-gen.mjs [--write]`（缺省 dry-run，只打印统计与将变状态）
 * 判据：`pnpm tools lift check` 不比表（它太大）；表的正确性由 CTest `strfmt` 的定点用例守
 *       （0x82A0=あ / 0x835C=ソ / 半角片假名 / 不可编码 ⇒ '?' / 8 个单字节例外）。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';

import { REPO_ROOT } from './lib/paths.mjs';

const OUT = 'apps/emulator-lift/src/runtime/cp932_table.h';
const DEC = new TextDecoder('shift_jis', { fatal: true });

/** 与 tools/lib/cp932.mjs 的 CP932_SINGLE 完全一致（Windows CP932 与 WHATWG 的 8 个单字节差异） */
const CP932_SINGLE = new Map([
  [0x1a, 0x001a],
  [0x1c, 0x001c],
  [0x7f, 0x007f],
  [0x80, 0x0080],
  [0xa0, 0xf8f0],
  [0xfd, 0xf8f1],
  [0xfe, 0xf8f2],
  [0xff, 0xf8f3],
]);
const isLead = (b) => (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
const isTrail = (b) => (b >= 0x40 && b <= 0x7e) || (b >= 0x80 && b <= 0xfc);

function build() {
  // 单字节：0..255；0xFFFF = 不可解
  const single = new Array(256).fill(0xffff);
  for (let b = 0; b < 256; b++) {
    if (CP932_SINGLE.has(b)) {
      single[b] = CP932_SINGLE.get(b);
      continue;
    }
    if (b < 0x80 || (b >= 0xa1 && b <= 0xdf)) {
      try {
        const s = DEC.decode(Uint8Array.of(b));
        if (s.length === 1) single[b] = s.charCodeAt(0);
      } catch {
        /* 不可解 */
      }
    }
  }
  // 双字节：lead×(trail)；表项 = (lead<<24)|(trail<<16)|codeUnit
  const pairs = [];
  for (let lead = 0; lead < 256; lead++) {
    if (!isLead(lead)) continue;
    for (let trail = 0; trail < 256; trail++) {
      if (!isTrail(trail)) continue;
      let s;
      try {
        s = DEC.decode(Uint8Array.of(lead, trail));
      } catch {
        continue;
      }
      if (s.length !== 1) continue; // CP932 只有 BMP 单码元
      pairs.push(((lead << 24) | (trail << 16) | s.charCodeAt(0)) >>> 0);
    }
  }
  pairs.sort((a, b) => a - b);
  return { single, pairs };
}

function render({ single, pairs }) {
  const L = [];
  L.push('// emulator-lift · CP932 码表（**派生文件，⛔ 不要手改**）');
  L.push('//');
  L.push('// 生成：`node tools/lift-cp932-gen.mjs --write`（tools/lift.mjs 之外唯一允许的 lift* 工具）');
  L.push('// 口径：双字节与 WHATWG shift_jis 逐对一致；单字节按 Windows CP932 的 8 个差异修正');
  L.push('//       （与 tools/lib/cp932.mjs 的 CP932_SINGLE 同一份口径，见该文件头注释）。');
  L.push('// 为什么入库：plan §7.1 —— 三平台必须**逐字节一致**，不能各用各的宿主码页设施；');
  L.push('//               也避免 libiconv 的 LGPL 静态链接义务（备选方案的代价）。');
  L.push(`// 规模：单字节表 256 项（有效 ${single.filter((x) => x !== 0xffff).length}），双字节 pair ${pairs.length} 个。`);
  L.push('#pragma once');
  L.push('');
  L.push('#include <cstdint>');
  L.push('');
  L.push('namespace lift {');
  L.push('namespace cp932_table {');
  L.push('');
  L.push('// 0xFFFF = 这个单字节在 CP932 里没有定义');
  L.push('inline constexpr std::uint16_t kSingle[256] = {');
  for (let i = 0; i < 256; i += 8) {
    const row = single.slice(i, i + 8).map((v) => `0x${v.toString(16).padStart(4, '0')}`).join(', ');
    L.push(`    ${row},  // 0x${i.toString(16).padStart(2, '0')}..`);
  }
  L.push('};');
  L.push('');
  L.push('// 表项 = (lead << 24) | (trail << 16) | UTF-16 码元；按 (lead, trail) 升序。');
  L.push(`inline constexpr std::uint32_t kPairs[${pairs.length}] = {`);
  for (let i = 0; i < pairs.length; i += 6) {
    const row = pairs.slice(i, i + 6).map((v) => `0x${v.toString(16).padStart(8, '0')}u`).join(', ');
    L.push(`    ${row},`);
  }
  L.push('};');
  L.push('');
  L.push('}  // namespace cp932_table');
  L.push('}  // namespace lift');
  L.push('');
  return L.join('\n');
}

export const DOMAIN = {
  id: 'lift-cp932',
  title: 'emulator-lift · **CP932 码表派生**（`cp932_table.h`；唯一写入口）',
  data: [
    '`apps/emulator-lift/src/runtime/cp932_table.h`（**派生、入库**：单字节 256 + 双字节 9604 对）',
  ],
  access: '只读宿主 Node 的 TextDecoder（shift_jis）+ 8 个 CP932 修正；`--write` 才落盘（写后回读复验）',
  tool: 'tools/lift-cp932-gen.mjs',
};

export const OPERATIONS = [
  { name: 'generate', argv: ['--generate'], mutates: true, summary: '重算并比对表；缺省 dry-run，`--write` 落盘（写后回读复验）' },
];

export async function main(argv = process.argv.slice(2)) {
  const write = argv.includes('--write');
  const { single, pairs } = build();
  const text = render({ single, pairs });
  const p = path.join(REPO_ROOT, OUT);
  const old = fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : '';
  console.log(`cp932: 单字节有效 ${single.filter((x) => x !== 0xffff).length}/256，双字节 pair ${pairs.length}`);
  console.log(`${OUT}: ${old === text ? '已是最新' : '将变'}`);
  if (write) {
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, text);
    if (fs.readFileSync(p, 'utf8') !== text) throw new Error('写后回读不一致');
    console.log('已写入（写后回读一致）。');
  }
  return 0;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
if (isDirectRun) {
  main()
    .then((code) => {
      process.exitCode = code ?? 0;
    })
    .catch((err) => {
      process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
      process.exitCode = 2;
    });
}
