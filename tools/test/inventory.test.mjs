/** @env pure @kind gate @why 盘点生成器静默产出错数字（不许"读不到就当空"） */
/**
 * tools/test/inventory.test.mjs — 旧仓盘点生成器的两条**基建契约**（都不需要旧仓在场）
 *
 * ① **不许静默写出错盘点**：git 事实拿不到时必须**抛错**，而不是"读不到就当空"生成一份全 0 的 md
 *    （实测踩过：受限沙箱里管道的 `spawn EPERM` 被 `catch` 吞掉，把好盘点覆盖成 0 文件/0 提交）。
 * ② **对旧仓严格只读**：`runCapture` 的临时文件写在系统临时区，不往被调用的仓库里写任何东西。
 *
 * ★ 原先还有第三条"真旧仓可盘点" —— 它要**旧仓在场** ⇒ 已移到 `inventory.external.test.mjs`
 *   （`@env external`）。顺带修掉一个**假绿**：那条原来写 `t.diagnostic(...)` + 裸 `return`，
 *   而 node:test 把那种写法记成 **pass**（实测），于是"没跑"与"跑了且绿"长得一模一样。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { collect } from '../old-repo-inventory.mjs';

const tmpDir = () => fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-inv-'));

test('★ 非 git 目录 ⇒ collect() 必须抛错（不许静默给 0）', () => {
  const dir = tmpDir();
  assert.throws(() => collect(dir, { coupling: false }), /跑不起来|git/, '拿不到 git 事实时必须抛错');
});

test('★ 盘点生成器不会往被调用的目录里写东西（旧仓只读）', () => {
  const dir = tmpDir();
  const before = fs.readdirSync(dir).sort();
  try {
    collect(dir, { coupling: false });
  } catch {
    /* 预期抛错 */
  }
  assert.deepEqual(fs.readdirSync(dir).sort(), before, '被调用的目录里不许出现任何新文件（含 .tmp/）');
});
