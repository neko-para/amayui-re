/** @env pure @kind gate @why 三处 pl_mpeg 副本漂了 ⇒ 同一部影片在 Electron / C++ 对照实现 / emulator-lift 上会解出不同像素，而三边的其它测试都仍然绿 */
/**
 * tools/test/emulator-cxx-pl-mpeg-sync.test.mjs —— 第三方解码器的**同一份字节**
 *
 * 本仓有三处 `pl_mpeg.h`：
 *   · `apps/emulator-new/src/platform/electron/renderer/movie/pl_mpeg.h`（编译成 `movie.wasm`）
 *   · `apps/emulator-cxx/third_party/pl_mpeg/pl_mpeg.h`（C++ 对照实现直接编译）
 *   · `apps/emulator-lift/third_party/pl_mpeg/pl_mpeg.h`（当前实现线：静态库 `lift-pl-mpeg`）
 *
 * 判据（为什么会红）：
 *   ① 三份**逐字节相同** —— 否则"同一部影片、同一版解码器"这句话不成立；
 *      只改一边时，另外两边的影片回归测试仍然是绿的（它们各自用自己那份）。
 *   ② 仍是上游 pl_mpeg（文件头带 SPDX MIT 标识）—— 防止有人把副本换成别的库再声称"同一份"。
 *
 * 为什么不是"允许各自补丁"：三处都只被各自的包装层使用（`movie.c` / `core/movie.cpp` /
 * `runtime/video/movie_decoder.cpp`），补丁写在包装层就够了；改副本会让升级与来源说明都失去
 * 唯一性（见 `third_party/pl_mpeg/README.md`）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const WASM_COPY = path.join(
  REPO_ROOT,
  'apps/emulator-new/src/platform/electron/renderer/movie/pl_mpeg.h',
);
const CXX_COPY = path.join(REPO_ROOT, 'apps/emulator-cxx/third_party/pl_mpeg/pl_mpeg.h');
const LIFT_COPY = path.join(REPO_ROOT, 'apps/emulator-lift/third_party/pl_mpeg/pl_mpeg.h');

const COPIES = [
  { label: 'Electron（movie.wasm）', file: WASM_COPY },
  { label: 'emulator-cxx（C++ 对照实现）', file: CXX_COPY },
  { label: 'emulator-lift（当前实现线）', file: LIFT_COPY },
];

const sha256 = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

test('★ 三处 pl_mpeg.h 逐字节相同，且仍是上游 MIT 单文件库', () => {
  for (const { label, file } of COPIES) {
    assert.ok(fs.existsSync(file), `缺文件（${label}）：${file}`);
  }
  const wasm = fs.readFileSync(WASM_COPY);
  const reference = sha256(wasm);
  for (const { label, file } of COPIES.slice(1)) {
    assert.equal(
      sha256(fs.readFileSync(file)),
      reference,
      `${label} 的 pl_mpeg.h 与 Electron 那份不一致（升级时三处一起换；本仓要求逐字节相同）`,
    );
  }
  // 上游身份：SPDX 标识 + 文件头里的项目名。
  const head = wasm.subarray(0, 512).toString('latin1');
  assert.match(head, /SPDX-License-Identifier: MIT/);
  assert.match(head, /PL_MPEG|MPEG1 Video decoder/);
});
