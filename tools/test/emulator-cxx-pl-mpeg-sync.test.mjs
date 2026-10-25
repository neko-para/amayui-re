/** @env pure @kind gate @why 两处 pl_mpeg 副本漂了 ⇒ 同一部影片在 Electron 与 C++ 宿主上会解出不同像素，而两边的其它测试都仍然绿 */
/**
 * tools/test/emulator-cxx-pl-mpeg-sync.test.mjs —— 第三方解码器的**同一份字节**
 *
 * 本仓有两处 `pl_mpeg.h`：
 *   · `apps/emulator-new/src/platform/electron/renderer/movie/pl_mpeg.h`（编译成 `movie.wasm`）
 *   · `apps/emulator-cxx/third_party/pl_mpeg/pl_mpeg.h`（C++ 宿主直接编译）
 *
 * 判据（为什么会红）：
 *   ① 两份**逐字节相同** —— 否则"同一部影片、同一版解码器"这句话不成立；
 *      只改一边时，另一边的影片回归测试仍然是绿的（它们各自用自己那份）。
 *   ② 仍是上游 pl_mpeg（文件头带 SPDX MIT 标识）—— 防止有人把副本换成别的库再声称"同一份"。
 *
 * 为什么不是"允许各自补丁"：两处都只被各自的包装层使用（`movie.c` / `core/movie.cpp`），
 * 补丁写在包装层就够了；改副本会让升级与来源说明都失去唯一性（见 `third_party/pl_mpeg/README.md`）。
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

const sha256 = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

test('★ 两处 pl_mpeg.h 逐字节相同，且仍是上游 MIT 单文件库', () => {
  for (const file of [WASM_COPY, CXX_COPY]) {
    assert.ok(fs.existsSync(file), `缺文件：${file}`);
  }
  const wasm = fs.readFileSync(WASM_COPY);
  const cxx = fs.readFileSync(CXX_COPY);
  assert.equal(
    sha256(cxx),
    sha256(wasm),
    'apps/emulator-cxx 的 pl_mpeg.h 与 Electron 那份不一致（升级时两份一起换）',
  );
  // 上游身份：SPDX 标识 + 文件头里的项目名。
  const head = cxx.subarray(0, 512).toString('latin1');
  assert.match(head, /SPDX-License-Identifier: MIT/);
  assert.match(head, /PL_MPEG|MPEG1 Video decoder/);
});
