/** @env assets @kind product @why 真实索引的发行默认表 / 玩家文件覆盖 / 默认文件生成任一处错，启动都会拿一份"看起来正常"的错配置（SCREENX=0、字体空、版本不变） */
/**
 * tools/test/emulator-new-config.assets.test.mjs —— 配置子系统在**真实安装**上的守卫。
 *
 * 1. `SYS4INI.BIN` 尾部真的带 51 条发行默认（SCREENX / SCREENY / FONT / SAVEVERSION）；
 * 2. 玩家文件缺失 ⇒ 默认；写出的默认文件能重放；
 * 3. 启动阶段 `display-config` = partial，并带配置摘要。
 *
 * ★ 样本缺席（fresh clone / 没装游戏）⇒ skip，不是红。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { loadManifest } from '../lib/manifest.mjs';
import { rootFromManifest } from '../lib/patch.mjs';
import { Install, readConfigFile, writeConfigFile } from '../../apps/emulator-new/src/host.ts';
import { ConfigStore, parseIni } from '../../apps/emulator-new/src/config.ts';
import { loadConfigKeys } from '../../apps/emulator-new/src/spec.ts';
import { analyzeInstall } from '../../apps/emulator-new/src/main.ts';

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

const TEMP = path.join(REPO_ROOT, '.tmp', 'emulator-new-config-assets');

test('★ 真实索引：51 条发行默认（SCREENX/SCREENY/FONT/SAVEVERSION）', { skip }, async () => {
  const install = Install.open(installDir);
  assert.equal(install.index.configDefaults.length, 51, 'SYS4INI.BIN 尾部应有 51 对默认值');
  const keys = install.index.configDefaults.map((x) => x.key);
  for (const k of ['SCREENX', 'SCREENY', 'FONT', 'SAVEVERSION', 'SAVEPATH', 'REGFILEPATH']) {
    assert.ok(keys.includes(k), `发行默认缺 ${k}`);
  }

  const s = new ConfigStore(loadConfigKeys());
  s.applyDefaults(install.index.configDefaults);
  assert.equal(s.getInt('set:WinX'), 1280);
  assert.equal(s.getInt('set:WinY'), 720);
  assert.equal(s.getInt('set:SaveVersion1'), 3);
  assert.equal(s.getInt('set:SaveVersion2'), 20);
  assert.equal(new TextDecoder('shift_jis').decode(Buffer.from(s.getString('message:Font'), 'latin1')), 'ＭＳ 明朝');
  assert.ok(new TextDecoder('shift_jis').decode(Buffer.from(s.getString('set:SavePath'), 'latin1')).endsWith('\\SAVE'));

  // 没有尾部的索引（AAI）⇒ 空默认表，不是"解析失败"
  const aai = fs.readdirSync(installDir).find((n) => /^APPEND\d+\.AAI$/i.test(n));
  if (aai) {
    const buf = fs.readFileSync(path.join(installDir, aai));
    // 直接复用纯解析器：AAI 的 AC 段起点不同，但尾部为空
    const { parseAlfIndex } = await import('../../apps/emulator-new/src/alf.ts');
    assert.deepEqual(parseAlfIndex(buf).configDefaults, []);
  }
});

test('★ 玩家文件缺失 ⇒ 默认；--init-config 写出的文件可重放', { skip }, () => {
  fs.rmSync(TEMP, { recursive: true, force: true });
  fs.mkdirSync(TEMP, { recursive: true });
  const file = path.join(TEMP, 'SYS4REG.INI');
  assert.equal(readConfigFile(file), null, '文件不存在是正常状态');

  const install = Install.open(installDir);
  const s = new ConfigStore(loadConfigKeys());
  s.applyDefaults(install.index.configDefaults);
  assert.equal(s.getInt('display:ScreenMode'), 0);

  const before = analyzeInstall(installDir, 0, { decodeOnly: true }, { config: file });
  assert.equal(before.config.source, 'defaults');

  // CLI 的 --init-config 路径：显式路径 + 写默认文件
  const initFile = path.join(TEMP, 'INIT.INI');
  const init = analyzeInstall(installDir, 0, { decodeOnly: true }, { config: initFile, initConfig: true });
  assert.equal(init.config.path, initFile);
  assert.ok(fs.existsSync(initFile), '--init-config 必须落盘');
  assert.ok(fs.readFileSync(initFile, 'latin1').startsWith('[display]\r\n'));

  writeConfigFile(file, s.serializeIni());
  const text = fs.readFileSync(file, 'latin1');
  assert.ok(text.startsWith('[display]\r\n'), '默认文件以 display 段开头、CRLF');
  const again = new ConfigStore(loadConfigKeys());
  again.applyDefaults(install.index.configDefaults);
  again.applyIni(parseIni(fs.readFileSync(file)));
  assert.equal(again.getInt('set:WinX'), 1280);
  assert.equal(again.getInt('display:ScreenMode'), 0);

  const after = analyzeInstall(installDir, 0, { decodeOnly: true }, { config: file });
  assert.equal(after.config.source, 'file');
  assert.ok(after.config.ints >= s.stats().ints, '玩家文件写入后条目只会变多');
});

test('★ 启动阶段：display-config 已部分建模', { skip }, () => {
  // 显式给一个不存在的玩家文件：不依赖安装根里是否恰好有一份 SYS4REG.INI
  const missing = path.join(TEMP, 'ABSENT.INI');
  const r = analyzeInstall(installDir, 0, { decodeOnly: true }, { config: missing });
  assert.ok(r.config, 'install 报告必须带配置摘要');
  assert.equal(r.config.source, 'defaults');
  assert.equal(r.config.path, missing);
  const stage = r.startup.stages.find((x) => x.id === 'display-config');
  assert.equal(stage.disposition, 'partial');
  assert.equal(r.startup.config.path, r.config.path);
});
