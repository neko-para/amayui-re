/** @env pure @kind gate @why renderer 的 Host 路径实现是 IPC 边界：拼错会让安装取件 / 配置目录在 macOS / Windows 上分叉 */
/**
 * `apps/emulator-new/src/platform/electron/renderer/paths.ts` 的纯函数守卫。
 *
 * ★ 为什么需要它：core 的 `Host.join` / `dirname` 是同步的，而 renderer 与 main 的 IPC 是异步的
 *   ⇒ renderer 只能自己按 main 报告的 `sep` 做字符串运算。这里拼错不会报错，只会让
 *   `SYS4INI.BIN` / `SYS4REG.INI` 落到别的路径（尤其 Windows 的 `\` 与 CP932 的 trail byte 0x5C）。
 * ★ 只测纯函数；文件读写仍由 Electron smoke / 真安装跑。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { dirnamePath, joinPath, pathFromEngine } from '../../apps/emulator-new/src/platform/electron/renderer/paths.ts';

test('join：POSIX / Windows 的绝对路径、相对段与 ..', () => {
  assert.equal(joinPath('/', '/a', 'b', 'c'), '/a/b/c');
  assert.equal(joinPath('/', '/a/b', '..', 'c'), '/a/c');
  assert.equal(joinPath('/', 'a', '..', '..', 'b'), '../b');
  assert.equal(joinPath('/', '', 'x'), 'x');
  assert.equal(joinPath('\\', 'C:\\Games', 'DATA01.ALF'), 'C:\\Games\\DATA01.ALF');
  assert.equal(joinPath('\\', 'C:\\Games\\x', '..', 'y'), 'C:\\Games\\y');
});

test('dirname：根 / 普通文件 / Windows 盘符', () => {
  assert.equal(dirnamePath('/', '/a/b'), '/a');
  assert.equal(dirnamePath('/', '/a'), '/');
  assert.equal(dirnamePath('/', 'a'), '.');
  assert.equal(dirnamePath('\\', 'C:\\a\\b'), 'C:\\a');
  assert.equal(dirnamePath('\\', 'C:\\a'), 'C:\\');
});

test('pathFromEngine：CP932 先解码，再把反斜杠换成宿主分隔符', () => {
  // `Eushully\天結いキャッスルマイスター` 的 CP932 字节（latin1 字符）
  const engine = 'Eushully\\\x93V\x8C\x8B\x82\xA2\x83L\x83\x83\x83b\x83X\x83\x8B\x83}\x83C\x83X\x83^\x81[';
  assert.equal(pathFromEngine('/', engine), 'Eushully/天結いキャッスルマイスター');
  assert.equal(pathFromEngine('\\', engine), 'Eushully\\天結いキャッスルマイスター');
});
