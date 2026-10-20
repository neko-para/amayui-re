/** @env assets @kind product @why 真实入口退化成"直接读某个脚本文件"（索引没解析 / id 映射错 / 松散优先丢了），而所有 pure 测试仍然绿 */
/**
 * tools/test/emulator-new-boot.assets.test.mjs —— **真实入口**（安装目录 → SYS4INI.BIN → 统一文件 id）的守卫
 *
 * ## 判据是什么
 * 1. 索引是真的：`S4IC` · 8 个归档 · 21109 条目；条目 0 = `SYSTEM4.BIN`，条目 `0x5262` = `LOGO.BIN`；
 * 2. 取件顺序是真的：根脚本（id 0）在这台安装上有**松散文件**（12012 字节）覆盖归档条目（11992 字节）；
 *    `LOGO.BIN` 没有松散文件 ⇒ 从 `DATA*.ALF` 切片（长度 1508 / sha256 钉住）；
 * 3. 启动是真的：以 **id 0** 为根脚本执行（不是直接读 LOGO.BIN），停在启动前段第一个未实现 opcode 前。
 *    ★ 这个停点是**棘轮**：实现启动前段的新 opcode 时它会往后走，那时同步抬高这里。
 *
 * ★ 样本缺席（fresh clone / 没装游戏）⇒ skip，不是红。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { loadManifest } from '../lib/manifest.mjs';
import { rootFromManifest } from '../lib/patch.mjs';
import { Install } from '../../apps/emulator-new/src/host.ts';
import { analyzeInstall } from '../../apps/emulator-new/src/main.ts';
import { BOOT_STAGES } from '../../apps/emulator-new/src/boot.ts';

const KEY = 0x1234abcd;

const manifest = loadManifest();
let installDir = null;
try {
  installDir = rootFromManifest(manifest, 'gameInstall');
} catch {
  installDir = null;
}
const skip = installDir && fs.existsSync(path.join(installDir, 'SYS4INI.BIN'))
  ? false
  : '游戏安装不在场（corpus/assets.json 的 roots.gameInstall 下没有 SYS4INI.BIN）';

test('★ 索引：SYS4INI.BIN → 8 个归档 / 21109 条目；id 0 = SYSTEM4.BIN，id 0x5262 = LOGO.BIN', { skip }, () => {
  const install = Install.open(installDir);
  assert.equal(install.index.signature, 'S4IC');
  assert.equal(install.index.archives.length, 8);
  assert.deepEqual(install.index.archives.slice(0, 2), ['DATA1.ALF', 'DATA2.ALF']);
  assert.equal(install.entryCount, 21109);

  assert.equal(install.entry(0).name, 'SYSTEM4.BIN', '根脚本 = 统一文件 id 0');
  assert.equal(install.entry(0x5262).name, 'LOGO.BIN');
  assert.equal(install.entry(0x5263).name, 'INIT.BIN');
  assert.equal(install.entry(0x5264).name, 'TITLE.BIN');
  assert.equal(install.entry(0x5258).name, 'LOADCONFIG.BIN');
  assert.equal(install.entry(0x51db).name, 'CHECKCONFIG.BIN');
  assert.equal(install.entry(0x51dc).name, 'INITCONFIG.BIN');
});

test('★ 取件：根脚本松散优先（12012）覆盖归档条目（11992）；LOGO.BIN 从归档切片', { skip }, () => {
  const install = Install.open(installDir);

  const loose = install.readById(0);
  assert.equal(loose.side, 'loose', '这台安装的根脚本必须命中松散文件（否则启动链量的是另一份没人跑的文件）');
  assert.equal(loose.entryLength, 11992, 'TOC 登记的长度');
  assert.equal(loose.bytes.length, 12012, '松散文件的真实字节数');

  const archived = install.readArchiveById(0);
  assert.equal(archived.side, 'archive');
  assert.equal(archived.bytes.length, 11992);
  assert.equal(archived.bytes.toString('latin1', 0, 4), 'SYS4');
  assert.notEqual(archived.bytes.toString('base64'), loose.bytes.subarray(0, archived.bytes.length).toString('base64'),
    '松散文件与归档条目必须是两份不同的内容（否则"松散优先"无从验证）');

  const logo = install.readById(0x5262);
  assert.equal(logo.side, 'archive');
  assert.equal(logo.name, 'LOGO.BIN');
  assert.equal(logo.entryLength, 1508);
  assert.equal(crypto.createHash('sha256').update(logo.bytes).digest('hex'),
    'f17364da0ffbca38aedee4a3b46db15945a7cf864e3b7773a422d24fa6b9ba9b');
});

test('★ 启动棘轮：以 id 0（SYSTEM4.BIN）为根执行，停在启动前段第一个未实现 opcode 前', { skip }, () => {
  const result = analyzeInstall(installDir, 0, { key: KEY }, { config: path.join(REPO_ROOT, '.tmp', 'boot-config-missing.ini') });
  assert.equal(result.source.kind, 'install');
  assert.equal(result.source.id, 0);
  assert.equal(result.source.name, 'SYSTEM4.BIN');
  assert.equal(result.source.side, 'loose');
  assert.equal(result.source.indexEntries, 21109);
  assert.equal(result.decoded, 545, '松散 SYSTEM4.BIN 的指令数（结构基准）');
  // 启动阶段记账必须随报告一起出来（跳过不许静默）
  assert.deepEqual(result.startup?.stages.map((s) => s.id), BOOT_STAGES.map((s) => s.id));
  assert.equal(result.startup?.counts['not-modeled'], 0, '配置补上后不应再有完全未建模的阶段');
  assert.equal(result.config?.source, 'defaults', '安装根没有 SYS4REG.INI ⇒ 发行默认');
  assert.equal(result.config?.path, path.join(REPO_ROOT, '.tmp', 'boot-config-missing.ini'));

  assert.equal(result.mode, 'run');
  assert.equal(result.steps, 85, '当前启动前段：… / 0x3 call-script → LOADCONFIG.BIN 内执行到 0x192');
  assert.equal(result.stop, 'unimplemented');
  assert.equal(result.gap?.opcode, 0x192, '★ 缺口：子脚本 LOADCONFIG.BIN 偏移 192 的 0x192（argc 2）');
  assert.equal(result.gap?.index, 85, '已执行 85 条');
  assert.equal(result.gap?.offset, 192, '子脚本内的偏移');
});

test('★ 同一入口按 id 取 LOGO.BIN：50 条指令（id 映射不是巧合）', { skip }, () => {
  const result = analyzeInstall(installDir, 0x5262, { decodeOnly: true });
  assert.equal(result.source.id, 0x5262);
  assert.equal(result.source.name, 'LOGO.BIN');
  assert.equal(result.source.entryLength, 1508);
  assert.equal(result.decoded, 50);
  assert.equal(result.stop, 'code-region-end');
});














