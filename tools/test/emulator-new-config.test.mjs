/** @env pure @kind contract @why 配置库的默认表归一化 / 玩家文件覆盖 / 保存格式 / 四条脚本访问写错了（键错、默认值错、段错），启动与脚本都会"读到一个看起来正常的错值" */
/**
 * tools/test/emulator-new-config.test.mjs —— `55-config.md` 的 pure 守卫。
 *
 * 不读游戏安装：键词表来自 `data/spec/format/config-keys.json`，输入都是合成字节 / 合成池。
 * 真实索引的 51 条默认表由 `emulator-new-config.assets.test.mjs` 覆盖。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

import { ConfigStore, atoiPrefix, parseIni } from '../../apps/emulator-new/src/config.ts';
import { defaultConfigPath } from '../../apps/emulator-new/src/host.ts';
import { loadConfigKeys } from '../../apps/emulator-new/src/spec.ts';
import { decodeInt, encodeInt, readText, writeText } from '../../apps/emulator-new/src/values.ts';
import { Machine } from '../../apps/emulator-new/src/machine.ts';
import { slotAddress } from '../../apps/emulator-new/src/address.ts';
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

  // 收尾修补 / 派生键 / 接受即忽略 / 列表清空
  const repair = store();
  repair.applyDefaults([
    { key: 'MENU_SAVE', value: '0' },
    { key: 'SAVEVERSION', value: '100' },
  ]);
  assert.equal(repair.getInt('set:SaveVersion1'), 1);
  assert.equal(repair.getInt('set:SaveVersion2'), 10, 'MENU_SAVE=0 + 1.00 ⇒ 1.10');

  const derived = store();
  derived.applyDefaults([
    { key: 'WHEELKEYUP', value: '5' },
    { key: 'OUTERFRAMEMODE', value: '2' },
  ]);
  assert.equal(derived.getInt('message:WheelKeyUpOnTW'), 5);
  assert.equal(derived.getInt('set:WheelKeyUp'), 5);

  const twice = store();
  twice.applyDefaults([{ key: 'VALIDFONTCHECKER', value: 'A' }]);
  twice.applyDefaults([{ key: 'VALIDFONTCHECKER', value: 'B' }]);
  assert.deepEqual(twice.getList('list:ValidFontChecker'), ['B'], '每次解析前清空列表');

  const wide = store();
  wide.applyDefaults([{ key: 'REGKEY', value: '4294967297' }]);
  assert.equal(wide.getInt('set:RegKey'), 1, '_atoi64 后只存低 32 位');

  const anti = store();
  anti.applyDefaults([{ key: 'ENABLEANTIFONT', value: '2' }], { antifontUnavailable: true });
  assert.equal(anti.getInt('set:EnableAntiFont'), 2, '只有值 1 才做平台清零');

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
  // 空行也是一条“没有 = 的行” ⇒ 空键/空值；文件末尾的 LF 不是额外一行
  assert.equal(parseIni(Buffer.alloc(0)).size, 0, '空文件没有条目');
  assert.equal(parseIni(Buffer.from('[display]\r\n\r\nScreenMode=2\r\n', 'latin1')).get('display\\'), '');
  assert.equal(parseIni(Buffer.from('[display]\r\nScreenMode=2\r\n', 'latin1')).size, 1, '末尾 LF 不产生幻影空行');
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

  // 反向：开关打开时条件键必须出现（否则“条件”会退化成“永远不写”）
  const open = store();
  open.applyDefaults([]);
  open.setInt('display:VirtualFullScreen', 1);
  open.setInt('display:VirtualFullScreenType', 3);
  open.setInt('set:EnableAntiFont', 1);
  open.setInt('message:UseAntiFont', 1);
  open.setInt('message:AntiFontLevel', 2);
  const openText = open.serializeIni().toString('latin1');
  assert.match(openText, /^VirtualFullScreenType=3\r$/m, 'VirtualFullScreen≠0 ⇒ 写出 VirtualFullScreenType');
  assert.match(openText, /^UseAntiFont=1\r$/m, 'EnableAntiFont≠0 ⇒ 写出 UseAntiFont');
  assert.match(openText, /^AntiFontLevel=2\r$/m, 'EnableAntiFont≠0 ⇒ 写出 AntiFontLevel');

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
  assert.ok(s.ints.has('\u0003' + '00000007'), '整数键必须带类型码 \\x03 + 8 位十六进制');
  assert.ok(!s.ints.has('7') && !s.ints.has('300000007'), '键不是裸十进制 / 漏类型码');

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
  assert.ok(s.strings.has('\u0005' + '00000003'), '字符串键必须带类型码 \\x05 + 8 位十六进制');

  const empty = op(5, 9);
  m.globalString.set(9, 'old');
  HANDLERS.get(0x1aa)(m, [empty]);
  assert.equal(m.globalString.get(9), '', '未命中字符串 ⇒ 空串');
});

test('★ 脚本配置：指针族的键由**地址换算**；地址没写过（0）⇒ 响亮失败，不许当成"键下标 0"', () => {
  const m = machine(store());
  // 指针槽没写过 ⇒ 地址 0 ⇒ 不落在任何池窗口（整数侧）/ 低于该族全局池起点（字符串侧）
  assert.throws(() => HANDLERS.get(0x1a3)(m, [op(12, 0)]), /不落在任何池窗口|未定义行为|低于该族全局池起点/);
  assert.throws(() => HANDLERS.get(0x1aa)(m, [op(14, 0)]), /不落在任何池窗口|未定义行为|低于该族全局池起点/);
  // 真正不支持的类型仍然抛「操作数类型异常」（不是静默跳过）
  assert.throws(() => HANDLERS.get(0x1a3)(m, [op(9, 0)]), /config-key-unsupported-type/);
});

test('文本原语：只支持 type 5，其余类型响亮失败（不补空串）', () => {
  const m = machine(store());
  assert.equal(readText(m, [op(5, 3)], 1), '');
  writeText(m, [op(5, 3)], 1, 'x');
  assert.equal(m.globalString.get(3), 'x');
  assert.throws(() => readText(m, [op(6, 3)], 1), /text-unsupported-operand-type:0x6/);
  assert.throws(() => writeText(m, [op(11, 3)], 1, 'x'), /text-unsupported-operand-type:0xb/);
});

test('脚本配置：配置库缺席 ⇒ config-not-initialized（不静默 0）', () => {
  const m = new Machine({ body: Buffer.alloc(4), table: new Map(), key: 0 });
  assert.throws(() => HANDLERS.get(0x1a3)(m, [op(3, 7)]), /config-not-initialized/);
});

test('配置路径：USEAPPDATAFOLDER 选根，REGFILEPATH 拼相对目录', () => {
  const old = process.env.APPDATA;
  process.env.APPDATA = '/tmp/appdata-for-test';
  try {
    assert.equal(defaultConfigPath('/game', 'Eushully\\Game', true), path.join('/tmp/appdata-for-test', 'Eushully\\Game', 'SYS4REG.INI'));
    assert.equal(defaultConfigPath('/game', 'Eushully\\Game', false), path.join('/game', 'Eushully\\Game', 'SYS4REG.INI'));
  } finally {
    if (old === undefined) delete process.env.APPDATA;
    else process.env.APPDATA = old;
  }
});

test('atoiPrefix：C atoi 前缀语义', () => {
  assert.equal(atoiPrefix('   -12abc'), -12);
  assert.equal(atoiPrefix('+7'), 7);
  assert.equal(atoiPrefix('abc'), 0);
  assert.equal(atoiPrefix(''), 0);
  assert.equal(atoiPrefix('012'), 12);
  // 只有 C isspace（0x09–0x0D / 0x20）算前导空白；其它控制字节直接让 atoi 失败 ⇒ 0
  assert.equal(atoiPrefix('\t-7'), -7);
  assert.equal(atoiPrefix('\x01 42'), 0);
});

test('★★ 两个输入通道的大小写口径不同：索引默认表**折叠**大小写；玩家文件**字节精确**', () => {
  // ① 索引默认表侧：键名折叠大小写（引擎用 `__stricmp` 比，见 `.lst:004942FD/0049439E`）
  const s = store();
  s.applyDefaults([{ key: 'screenx', value: '1024' }]);
  assert.equal(s.getInt('set:WinX'), 1024, '默认表的小写键名也要命中（折叠大小写）');

  // ② 玩家文件侧：段名/键名按字节精确，大小写不同 ⇒ 忽略（不是"宽容入库"）
  const exact = store();
  exact.applyIni(new Map([['display\\UseDirectDrawEmuration', '1']]));
  assert.equal(exact.getInt('display:UseDirectDrawEmuration'), 1, '字节一致 ⇒ 命中');

  const wrong = store();
  wrong.applyIni(new Map([['Display\\usedirectdrawemuration', '1']]));
  assert.equal(wrong.getInt('display:UseDirectDrawEmuration'), 0, '★ 大小写不同 ⇒ 不命中');
});

test('★★ 配置键：指针**不落在该族全局池** ⇒ UB（响亮失败），⛔ 不许算出一个"取决于布局的键"', () => {
  const s = store();
  const m = machine(s);
  const KEY = 0;

  // 指向**局部** int 池第 3 格（type 12 槽里的地址）
  m.frame.localPtr.set(0, slotAddress(9, 3, m.cur));
  assert.throws(() => HANDLERS.get(0x1a2)(m, [op(12, 0)]), /未定义行为/,
    '局部池地址 ⇒ 键没有定义（引擎只做减法/除法）');

  // 指向全局**浮点**池（键族是整型 ⇒ 池不匹配）
  m.frame.localPtr.set(1, slotAddress(4, 2));
  assert.throws(() => HANDLERS.get(0x1a3)(m, [op(12, 1)]), /未定义行为/,
    '族不匹配（整型键 + 浮点池）同样是 UB');

  // 指向全局 int 池 ⇒ 有定义，键 = 该池的槽下标
  m.frame.localPtr.set(2, slotAddress(3, 5));
  m.globalInt.set(5, encodeInt(7, KEY));
  HANDLERS.get(0x1a2)(m, [op(12, 2)]);
  assert.ok(s.ints.has('\u0003' + '00000005'), '全局池指针 ⇒ 键下标 = 池下标');
});
