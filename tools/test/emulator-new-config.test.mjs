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

import { ConfigStore, atoiPrefix, parseIni } from '../../apps/emulator-new/src/core/config.ts';
import { decodeCp932 } from '../../apps/emulator-new/src/core/bytes.ts';
import { defaultConfigPath } from '../../apps/emulator-new/src/core/install.ts';
import { nodeHost } from '../../apps/emulator-new/src/platform/node/host.ts';
import { loadConfigKeys } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { decodeInt, encodeInt, readText, writeText } from '../../apps/emulator-new/src/core/values.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { slotAddress } from '../../apps/emulator-new/src/core/address.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';

/** 键词表是只读的规范数据：读一次，多个 store 共用（`ConfigStore` 不改 schema） */
const CONFIG_KEYS = await loadConfigKeys();

function store() {
  return new ConfigStore(CONFIG_KEYS);
}

function machine(s) {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: 0, config: s });
}

function op(type, raw) {
  return { type, raw };
}

test('★ 默认表：整数/字符串/列表/SAVEVERSION 拆分/未知键响亮失败', async () => {
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

  assert.throws(() => new ConfigStore(CONFIG_KEYS).applyDefaults([{ key: 'NO_SUCH_KEY', value: '1' }]), /config-unknown-key/);
});

test('★ INI：段与键字节精确、缺键保留默认、重复键最后一次', async () => {
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

test('★★ 段头边界：`]` 落在双字节字符内部不算结束括号；字符字节原样保留', async () => {
  // CP932：0x81 是前导字节、0x5D 落在后继字节区间（0x40–0x7E）⇒ 这一对是一个双字节字符。
  // 段名里出现「lead + 0x5D」时，`lastIndexOf(']')` 会把这个 0x5D 当结束括号、把字符截成半个；
  // 引擎的判据跳过字符内部的命中、继续向前找真 `]`。
  const decoyLast = parseIni(Buffer.from('[ab]c\x81\x5d\r\nScreenMode=2\r\n', 'latin1'));
  assert.equal(decoyLast.get('ab\\ScreenMode'), '2', '最后一个 `]` 在字符内部 ⇒ 取前一个真 `]`');
  assert.equal(decoyLast.has('ab]c\x81\\ScreenMode'), false, '⛔ 不许把双字节字符截掉半个');

  // 段名含「lead + 0x5D」、后面还有真正的 `]` ⇒ 取真 `]`，且字符字节原样留在段名里
  const both = parseIni(Buffer.from('[ab\x81\x5dcd]\r\nScreenMode=2\r\n', 'latin1'));
  assert.equal(both.get('ab\x81\x5dcd\\ScreenMode'), '2', '字符内部的 `]` 不参与端点判定，真 `]` 才结束');

  // 只剩字符内部的 `]` ⇒ 与「没有 `]`」同一条分支：段名取到行尾
  const none = parseIni(Buffer.from('[ab\x81\x5d\r\nScreenMode=2\r\n', 'latin1'));
  assert.equal(none.get('ab\x81\x5d\\ScreenMode'), '2', '没有落在字符外部的 `]` ⇒ 段名取到行尾');

  // ★ 边界判定是**局部**的（只看命中字节与相邻一字节），不按行首重新配对：
  // `[0x81 0x81 0x5D` 若按配对读，是「0x81 0x81 一个字符 + 独立的 0x5D」；
  // 但引擎只看 0x5D 的前一字节 0x81（前导）⇒ 把 0x5D 算在字符内部 ⇒ 它不是结束括号、段名取到行尾。
  const local = parseIni(Buffer.from('[\x81\x81\x5d\r\nScreenMode=2\r\n', 'latin1'));
  assert.equal(local.get('\x81\x81\x5d\\ScreenMode'), '2', '局部判据：0x5D 的前一字节是前导 ⇒ 落在字符内部');
  assert.equal(local.has('\x81\x81\\ScreenMode'), false, '⛔ 不按行首重新配对（配对式会得到段名 `\\x81\\x81`）');

  // 段头取**最后一个** `]`（不是第一个）：`[a]b]` ⇒ 段名 `a]b`
  const last = parseIni(Buffer.from('[a]b]\r\nScreenMode=2\r\n', 'latin1'));
  assert.equal(last.get('a]b\\ScreenMode'), '2', '段名 = 第一个 `[` 与最后一个 `]` 之间');
  assert.equal(last.has('a\\ScreenMode'), false, '⛔ 不是第一个 `]`（那是 `indexOf` 的行为）');

  // `lead + 0x5B` 在**行首**：原始首字节不是 `[` ⇒ 这一行不是段头（按普通条目解析）。
  // 段头的第一个 `[` 只能是第 0 字节、之前没有字节 ⇒ 它不可能落在双字节字符内部；
  // 所以只需给后向的 `]` 判双字节边界。
  const leadFirst = parseIni(Buffer.from('\x81\x5b\r\nScreenMode=2\r\n', 'latin1'));
  assert.equal(leadFirst.has('\\\x81\x5b'), true, '首字节是前导字节 ⇒ 不是段头');
  assert.equal(leadFirst.get('\\\x81\x5b'), '', '不带 `=` 的行：整行是键、值空串');
});

test('★★ 玩家文件字段裁剪：0x00–0x20 闭区间 / 只剥一层引号 / 引号内空格保留 / 无 `=` 行', async () => {
  // ① 裁剪集合 = 首尾 0x00–0x20 **闭区间**：0x20 要裁；0x21 以上不裁；0x80 以上（DBCS 前导字节）不裁。
  const pad = parseIni(Buffer.from('[display]\r\n\t\x01ScreenMode\x1f\t = \x1f 2 \t\r\n', 'latin1'));
  assert.equal(pad.get('display\\ScreenMode'), '2', '首尾 0x00–0x20（含 0x20 / 0x1F / 0x09 / 0x0D）都要裁掉');

  const bang = parseIni(Buffer.from('[display]\r\n!ScreenMode=2\r\n', 'latin1'));
  assert.equal(bang.has('display\\!ScreenMode'), true, '0x21 不是裁剪边界：`!` 留在键里');
  assert.equal(bang.has('display\\ScreenMode'), false, '不许把 0x21 也裁掉');

  const high = parseIni(Buffer.from('[display]\r\n\x80ScreenMode=2\r\n', 'latin1'));
  assert.equal(high.has('display\\\x80ScreenMode'), true, '0x80 以上（DBCS 前导字节）不是裁剪边界');

  // ② 只剥一层成对引号；引号内的空格保留；剥引号要求开引号前有被裁掉的字节（起始下标 > 0）
  const spaced = store();
  spaced.applyIni(parseIni(Buffer.from('[message]\r\nFont = "  A  "\r\n', 'latin1')));
  assert.equal(spaced.getString('message:Font'), '  A  ', '引号内的空格保留（先裁剪、后剥引号）');

  const one = store();
  one.applyIni(parseIni(Buffer.from('[message]\r\nFont = ""A""\r\n', 'latin1')));
  assert.equal(one.getString('message:Font'), '"A"', '只剥一层（不循环）');

  const bare = store();
  bare.applyIni(parseIni(Buffer.from('[message]\r\nFont="A"\r\n', 'latin1')));
  assert.equal(bare.getString('message:Font'), '"A"', '开引号前没有可裁字节 ⇒ 引号原样留下');

  const gapped = store();
  gapped.applyIni(parseIni(Buffer.from('[message]\r\nFont= "A" \r\n', 'latin1')));
  assert.equal(gapped.getString('message:Font'), 'A', '开引号前有空白 ⇒ 剥一层');

  const lone = store();
  lone.applyIni(parseIni(Buffer.from('[message]\r\nFont= "\r\n', 'latin1')));
  assert.equal(lone.getString('message:Font'), '', '单字符字段 ` "` 也剥（只按两个端点判，没有长度 ≥ 2 的前置条件）');

  // ③ 顶层 `=` 切分：值在第 2 个顶层 `=` 处截断；引号 / 括号内的 `=` 不是分隔符
  assert.equal(parseIni(Buffer.from('[message]\r\nFont=a=b\r\n', 'latin1')).get('message\\Font'), 'a', '值 = 第 1 字段（第 2 个顶层 `=` 处截断）');
  assert.equal(parseIni(Buffer.from('[message]\r\nFont="a=b"\r\n', 'latin1')).get('message\\Font'), '"a=b"', '引号内的 `=` 不是分隔符');
  assert.equal(parseIni(Buffer.from('[message]\r\nFont=(a=b)\r\n', 'latin1')).get('message\\Font'), '(a=b)', '括号内的 `=` 不是分隔符');
  assert.equal(parseIni(Buffer.from('[message]\r\n"Font=a\r\n', 'latin1')).get('message\\"Font=a'), '', '引号未配对（`=` 处奇数次）⇒ 该 `=` 不是顶层的 ⇒ 整行是键、值空串');

  // ④ 无 `=` 的行：键 = 整行（裁剪后）、值 = 空串；空行 ⇒ 空键空值；`;` / `#` 不是注释
  const noEq = parseIni(Buffer.from('[display]\r\nScreenMode\r\n', 'latin1'));
  assert.equal(noEq.has('display\\ScreenMode'), true, '不带 `=` 的非空行：整行（裁剪后）是键');
  assert.equal(noEq.get('display\\ScreenMode'), '', '…值是空串');
  assert.equal(noEq.has('display\\'), false, '…不是“空键”');

  const comment = parseIni(Buffer.from('[message]\r\n; x=1\r\n#y=2\r\n\r\nFont=A\r\n', 'latin1'));
  assert.equal(comment.get('message\\; x'), '1', '`;` 不是注释符：带 `=` 的注释行是普通条目');
  assert.equal(comment.get('message\\#y'), '2', '`#` 不是注释符');
  assert.equal(comment.get('message\\'), '', '空行 ⇒ 空键空值');

  // ⑤ 行读取的两条硬口径：行内 0x00 截断；没有 LF 结尾的末行被丢弃；读满 255 字节先交出一行
  assert.equal(parseIni(Buffer.from('[display]\r\nScreenMode=2\x00junk=9\r\n', 'latin1')).get('display\\ScreenMode'), '2', '行内 0x00 ⇒ 截断，其后不解析');
  assert.equal(parseIni(Buffer.from('[display]\r\nScreenMode=2', 'latin1')).size, 0, '没有 LF 结尾的末行被丢弃');
  const long = parseIni(Buffer.from(`${'A'.repeat(255)}=1\r\n`, 'latin1'));
  assert.equal(long.has(`\\${'A'.repeat(255)}`), true, '读满 255 字节先交出这一行');
  assert.equal(long.get('\\'), '1', '剩余字节（含紧随的 `=`）按下一行解析');
});

test('★ 保存：四段 CRLF 与条件键；重解析 round-trip', async () => {
  const s = store();
  s.applyDefaults([{ key: 'FULLSCREENBIT', value: '32' }]);
  s.setInt('display:ScreenMode', 2);
  s.setInt('display:VirtualFullScreen', 0);
  s.setInt('display:VirtualFullScreenType', 3);
  s.setInt('set:EnableAntiFont', 0);
  s.setInt('message:UseAntiFont', 1);
  const text = Buffer.from(s.serializeIni()).toString('latin1');
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
  const openText = Buffer.from(open.serializeIni()).toString('latin1');
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

test('★ 脚本配置四条：load-int 未命中写 0、save/load 往返、字符串空串', async () => {
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

test('★ 脚本配置：指针族的键由**地址换算**；地址没写过（0）⇒ 响亮失败，不许当成"键下标 0"', async () => {
  const m = machine(store());
  // 指针槽没写过 ⇒ 地址 0 ⇒ 不落在任何池窗口（整数侧）/ 低于该族全局池起点（字符串侧）
  assert.throws(() => HANDLERS.get(0x1a3)(m, [op(12, 0)]), /不落在任何池窗口|未定义行为|低于该族全局池起点/);
  assert.throws(() => HANDLERS.get(0x1aa)(m, [op(14, 0)]), /不落在任何池窗口|未定义行为|低于该族全局池起点/);
  // 真正不支持的类型仍然抛「操作数类型异常」（不是静默跳过）
  assert.throws(() => HANDLERS.get(0x1a3)(m, [op(9, 0)]), /config-key-unsupported-type/);
});

test('文本原语：支持 type 5 / 11，其余类型响亮失败（不补空串）', async () => {
  const m = machine(store());
  assert.equal(readText(m, [op(5, 3)], 1), '');
  writeText(m, [op(5, 3)], 1, 'x');
  assert.equal(m.globalString.get(3), 'x');
  writeText(m, [op(11, 3)], 1, 'local');
  assert.equal(readText(m, [op(11, 3)], 1), 'local', 'type 11：当前帧局部字符串池');
  assert.equal(m.frame.localString.get(3), 'local');
  assert.throws(() => readText(m, [op(6, 3)], 1), /text-unsupported-operand-type:0x6/);
});

test('脚本配置：配置库缺席 ⇒ config-not-initialized（不静默 0）', async () => {
  const m = new Machine({ body: Buffer.alloc(4), table: new Map(), key: 0 });
  assert.throws(() => HANDLERS.get(0x1a3)(m, [op(3, 7)]), /config-not-initialized/);
});

test('配置路径：USEAPPDATAFOLDER 选根，REGFILEPATH 拼相对目录', async () => {
  const old = process.env.APPDATA;
  process.env.APPDATA = '/tmp/appdata-for-test';
  try {
    assert.equal(defaultConfigPath(nodeHost, '/game', 'Eushully\\Game', true), path.join('/tmp/appdata-for-test', 'Eushully', 'Game', 'SYS4REG.INI'));
    assert.equal(defaultConfigPath(nodeHost, '/game', 'Eushully\\Game', false), path.join('/game', 'Eushully', 'Game', 'SYS4REG.INI'));
  } finally {
    if (old === undefined) delete process.env.APPDATA;
    else process.env.APPDATA = old;
  }
});

test('★ 引擎路径字节串：CP932 解码 + 反斜杠规范成宿主分隔符（不再是乱码目录）', async () => {
  // 原始字节 = CP932 的 `Eushully\天結いキャッスルマイスター`；核心口径是 latin1 字节串。
  const raw = Buffer.from('45757368756c6c795c93568c8b82a2834c838383628358838b837d83438358835e815b', 'hex');
  const enginePath = raw.toString('latin1');
  const decoded = '天結いキャッスルマイスター';
  assert.equal(nodeHost.pathFromEngine(enginePath), path.join('Eushully', decoded));
  const old = process.env.APPDATA;
  process.env.APPDATA = '/tmp/appdata-for-test';
  try {
    assert.equal(
      defaultConfigPath(nodeHost, '/game', enginePath, true),
      path.join('/tmp/appdata-for-test', 'Eushully', decoded, 'SYS4REG.INI'),
    );
  } finally {
    if (old === undefined) delete process.env.APPDATA;
    else process.env.APPDATA = old;
  }
  // 断言没有把 CP932 的 trail byte 0x5C 当分隔符切坏（解码后再替换）：
  // ★ 不能断言结果里没有 `\` —— Windows 的 path.sep 就是 `\`，那是**规范化后的宿主分隔符**。
  //   真正的不变量是"路径只分成 Eushully / 天結… 两段"（trail byte 被当分隔符会多切一段）。
  assert.equal(nodeHost.pathFromEngine(enginePath).split(path.sep).length, 2);
  // 显示层：同一套 CP932 字节串 → `メイリオ`（font.lookup 的 keyText 用）
  assert.equal(decodeCp932('\x83\x81\x83C\x83\x8a\x83I'), 'メイリオ');
});

test('atoiPrefix：C atoi 前缀语义', async () => {
  assert.equal(atoiPrefix('   -12abc'), -12);
  assert.equal(atoiPrefix('+7'), 7);
  assert.equal(atoiPrefix('abc'), 0);
  assert.equal(atoiPrefix(''), 0);
  assert.equal(atoiPrefix('012'), 12);
  // 只有 C isspace（0x09–0x0D / 0x20）算前导空白；其它控制字节直接让 atoi 失败 ⇒ 0
  assert.equal(atoiPrefix('\t-7'), -7);
  assert.equal(atoiPrefix('\x01 42'), 0);
});

test('★★ 两个输入通道的大小写口径不同：索引默认表**折叠**大小写；玩家文件**字节精确**', async () => {
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

test('★★ 配置键：指针**不落在该族全局池** ⇒ UB（响亮失败），⛔ 不许算出一个"取决于布局的键"', async () => {
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
