/** @env pure @kind contract @why 配置库的默认表归一化 / 玩家文件覆盖 / 保存格式 / 四条脚本访问写错了（键错、默认值错、段错），启动与脚本都会"读到一个看起来正常的错值" */
/**
 * tools/test/emulator-new-config.test.mjs —— `55-config.md` 的 pure 守卫。
 *
 * 不读游戏安装：键词表来自 `data/spec/format/config-keys.json`，输入都是合成字节 / 合成池。
 * 真实索引的 51 条默认表由 `emulator-new-config.assets.test.mjs` 覆盖。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ConfigStore, atoiPrefix, parseIni } from '../../apps/emulator-new/src/config.ts';
import { loadConfigKeys } from '../../apps/emulator-new/src/spec.ts';
import { decodeInt, encodeInt } from '../../apps/emulator-new/src/values.ts';
import { Machine } from '../../apps/emulator-new/src/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/handlers.ts';

function store() {
  return new ConfigStore(loadConfigKeys());
}

function machine(s) {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: 0, config: s });
}

function op(type, raw) {
  return { type, raw };
}

test('★ 默认表：整数/字符串/列表/SAVEVERSION 拆分/未知键响亮失败', () => {
  const s = store();
  const font = Buffer.from([0x82, 0x6c, 0x20, 0x96, 0xbe, 0x92, 0xa9]).toString('latin1');
  s.applyDefaults([
    { key: 'SCREENX', value: '1280' },
    { key: 'FONT', value: font },
    { key: 'VALIDFONTCHECKER', value: 'A' },
    { key: 'VALIDFONTCHECKER', value: 'B' },
    { key: 'SAVEVERSION', value: '320' },
    { key: 'NOSETMUSIC', value: '3' },
    { key: 'FORCESCREEN', value: '7' },
  ]);
  assert.equal(s.getInt('set:WinX'), 1280);
  assert.equal(s.getString('message:Font'), font);
  assert.deepEqual(s.getList('list:ValidFontChecker'), ['A', 'B']);
  assert.equal(s.getInt('set:SaveVersion1'), 3);
  assert.equal(s.getInt('set:SaveVersion2'), 20);
  assert.equal(s.getInt('set:NoSetMusic'), 3);
  assert.equal(s.getInt('sound:Music'), 2);
  assert.equal(s.getInt('display:ForceScreen'), 1);

  assert.throws(() => new ConfigStore(loadConfigKeys()).applyDefaults([{ key: 'NO_SUCH_KEY', value: '1' }]), /config-unknown-key/);
});

test('★ INI：段与键字节精确、缺键保留默认、重复键最后一次', () => {
  const s = store();
  s.applyDefaults([
    { key: 'SCREENX', value: '1280' },
    { key: 'FULLSCREENBIT', value: '32' },
  ]);
  s.applyIni(
    parseIni(
      Buffer.from(
        '[display]\r\nScreenMode=2\r\n  ScreenMode = "3" \r\nVirtualFullScreen=0\r\nVirtualFullScreenType=9\r\n[Display]\r\nScreenMode=1\r\n',
        'latin1',
      ),
    ),
  );
  assert.equal(s.getInt('display:ScreenMode'), 3, '重复键取最后一次；值两侧空白与引号要裁掉');
  assert.equal(s.getInt('display:VirtualFullScreenType'), 0, 'VirtualFullScreen=0 ⇒ 条件键不读');
  assert.equal(s.getInt('display:MultiSample'), 0, '文件里缺的键保留默认');
  assert.equal(s.getInt('display:FullScreenBit'), 32, '默认值不被无关段覆盖');
});

test('★ 保存：四段 CRLF 与条件键；重解析 round-trip', () => {
  const s = store();
  s.applyDefaults([{ key: 'FULLSCREENBIT', value: '32' }]);
  s.setInt('display:ScreenMode', 2);
  s.setInt('display:VirtualFullScreen', 0);
  s.setInt('display:VirtualFullScreenType', 3);
  s.setInt('set:EnableAntiFont', 0);
  s.setInt('message:UseAntiFont', 1);
  const text = s.serializeIni().toString('latin1');
  assert.ok(text.startsWith('[display]\r\n'), '第一段必须是 display，行尾 CRLF');
  const order = ['[display]', '[sound]', '[message]', '[system]'].map((x) => text.indexOf(x));
  assert.deepEqual(order, [...order].sort((a, b) => a - b), '段顺序固定');
  assert.ok(!text.includes('VirtualFullScreenType='), 'VirtualFullScreen=0 ⇒ 条件键不写');
  assert.ok(!text.includes('UseAntiFont='), 'EnableAntiFont=0 ⇒ 条件键不写');
  const bare = text.split('\r\n').join('');
  assert.ok(!bare.includes('\n'), '只许 CRLF，不许裸 LF');

  const again = store();
  again.applyDefaults([]);
  again.applyIni(parseIni(s.serializeIni()));
  assert.equal(again.getInt('display:ScreenMode'), 2);
  assert.equal(again.getInt('display:FullScreenBit'), 32);
  assert.equal(again.getInt('system:UseMMX'), 0);
});

test('★ 脚本配置四条：load-int 未命中写 0、save/load 往返、字符串空串', () => {
  const s = store();
  const m = machine(s);
  const slot = op(3, 7);
  m.globalInt.set(7, encodeInt(42, 0));
  HANDLERS.get(0x1a2)(m, [slot]);
  m.globalInt.set(7, 0);
  HANDLERS.get(0x1a3)(m, [slot]);
  assert.equal(decodeInt(m.globalInt.get(7), 0), 42, 'save-int → load-int 往返');

  const miss = op(3, 9);
  m.globalInt.set(9, encodeInt(5, 0));
  HANDLERS.get(0x1a3)(m, [miss]);
  assert.equal(decodeInt(m.globalInt.get(9), 0), 0, '未命中 ⇒ 0，且无条件写回');

  const str = op(5, 3);
  m.globalString.set(3, 'abc');
  HANDLERS.get(0x1a9)(m, [str]);
  m.globalString.set(3, 'zzz');
  HANDLERS.get(0x1aa)(m, [str]);
  assert.equal(m.globalString.get(3), 'abc', 'save-string → load-string 往返');

  const empty = op(5, 9);
  m.globalString.set(9, 'old');
  HANDLERS.get(0x1aa)(m, [empty]);
  assert.equal(m.globalString.get(9), '', '未命中字符串 ⇒ 空串');
});

test('★ 脚本配置：指针族操作数当前响亮失败（不是静默 0）', () => {
  const m = machine(store());
  assert.throws(() => HANDLERS.get(0x1a3)(m, [op(12, 0)]), /config-key-unsupported-type/);
  assert.throws(() => HANDLERS.get(0x1aa)(m, [op(14, 0)]), /config-key-unsupported-type/);
});

test('atoiPrefix：C atoi 前缀语义', () => {
  assert.equal(atoiPrefix('   -12abc'), -12);
  assert.equal(atoiPrefix('+7'), 7);
  assert.equal(atoiPrefix('abc'), 0);
  assert.equal(atoiPrefix(''), 0);
  assert.equal(atoiPrefix('012'), 12);
});
