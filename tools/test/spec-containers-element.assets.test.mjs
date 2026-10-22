/** @env assets @kind gate @why “写空容器抛 out_of_range”这一旧结论的语料判据：空 container 走追加分支、越界分支不可达；判据变了就说明该结论不再成立 */
/**
 * tools/test/spec-containers-element.assets.test.mjs —— 0x8003 写整型的**空容器分支**判据
 *
 * 旧台账 `KN-01M4GX1SDH4G0J2A5C4Q4M6P0Y` 曾称“写入空 vector 抛 `invalid vector<T> subscript`”。
 * 本守卫把语料控制流钉成机械判据：
 *   - `0x42B78E test ecx,0FFFFFFFCh` 判 (tail−head) 是否为 0；
 *   - `0x42B794 jnz 0x42B7D9`：**非 0 才跳**到越界检查；
 *   - `0x42B796` 紧随其后 = 空容器走**追加**；
 *   - `0x42B801 test edx,edx` / `0x42B803 jnz 0x42B80F` / `0x42B805 out_of_range`：
 *     到达这里时 edx = (tail−head)/4 ≥ 1 ⇒ `0x42B805` **不可达**。
 *
 * ★ 这不是语义证明：它只保证“旧结论所依赖的分支方向”没有变。
 * ★ 样本缺席 ⇒ skip，不是红。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');

function lineAt(ea) {
  const text = fs.readFileSync(LST, 'utf8');
  const prefix = `.text:${ea.toString(16).toUpperCase().padStart(8, '0')}`;
  const lines = text.split('\n').filter((l) => l.startsWith(prefix));
  return lines.map((l) => l.slice(prefix.length).trim()).filter(Boolean);
}

const skip = fs.existsSync(LST) ? false : '语料 .lst 不在场';

test('★ 空容器写整型：0x42B794 非 0 才跳到越界检查，空容器走 0x42B796 追加', { skip }, () => {
  const testLine = lineAt(0x42b78e).join(' | ');
  assert.match(testLine, /test\s+ecx,\s*0FFFFFFFCh/i, `0x42B78E 应是 (tail-head) 的非零测试：${testLine}`);
  const jnz = lineAt(0x42b794).join(' | ');
  assert.match(jnz, /jnz\s+.*42B7D9/i, `0x42B794 应是“非 0 才跳到 0x42B7D9”：${jnz}`);
  // 空容器不跳：0x42B794 是 2 字节短跳，落点就是 0x42B796。
  const append = lineAt(0x42b796).join(' | ');
  assert.ok(append.length > 0, '0x42B796 必须是一条指令（空容器追加路径）');
  assert.doesNotMatch(append, /^(?:loc_|def_|jpt_)/i, `0x42B796 不能只是标签：${append}`);
});

test('★ 越界分支：到达 0x42B801 时 edx 必然非 0 ⇒ 0x42B805 不可达', { skip }, () => {
  assert.match(lineAt(0x42b801).join(' | '), /test\s+edx,\s*edx/i, '0x42B801 应是 edx 非零测试');
  assert.match(lineAt(0x42b803).join(' | '), /jnz\s+.*42B80F/i, '0x42B803 应跳到合法写入 0x42B80F');
  assert.match(lineAt(0x42b805).join(' | '), /^push\b/i, '0x42B805 应是 out_of_range 的 push');
});
