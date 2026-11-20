/** @env pure @kind gate @why 冻结的配方指令闭集合 / 5 条 `repro:` 锚行 / `Step[]` 的名字源被改动（旧 plan 语义会静默漂） */
/**
 * tools/test/recipe-freeze.test.mjs —— **C3 的冻结面**守卫
 *
 * 合同：`docs/03-emulator/plugin-runtime.md` §5 C3（`:1166-1167` 与 §6 行 13/15/23）——
 *   `StepKind` / parser **冻结**：⛔ 不加 `global_set` / `global_get` / 变量 / 新指令；
 *   ⛔ 不做 JS→Step 生成。判据的另一半（语义 parity）在 CTest `recipe-driver-parity`。
 *
 * 红了几件事（每条都"变了就是 bug、而且变了是静默的"）：
 *   ① `StepKind` 的**闭集合**（10 个值、显式编号）被增 / 删 / 改名 ⇒ 旧 `.plan` 的语义面变了；
 *   ② `stepKindName()` 的**线上名字**（`await script` / `click` / …）被改 ⇒ `Step[]` 的 JSON
 *      （`src/runtime/recipe_json.cpp` 用 `stepKindName()` 的同一份名字）漂了，而 driver 是按
 *      这些字符串 switch 的（`scripts/plugins/lib/recipe.js` 的 `KIND_*`）⇒ 两侧会**静默**对不上；
 *   ③ `scripts/recipes/` 里 5 条需求树 `repro:` 锚行的**逐字**文本被改 ⇒ 锚悬空
 *      （需求单的写法是 `文件#子串`，改一个空格就解析不到）；
 *   ④ parser / 头里长出 `global_set` / `global_get` / `set_var` 这类**新指令** ⇒ 又给历史 plan 加能力。
 *
 * ⛔ **不断言"当前有几条配方"**（那是状态，会随下一次加配方而变错）；只断言"被点名的这 5 条锚行
 *   仍在它该在的文件里逐字在场"。
 * ⛔ 本文件不读 `data/**`（dev 树上的需求单是分叉快照；锚的**文本**在 `scripts/recipes/` 里）。
 *
 * 运行：`pnpm test`（`@env pure` ⇒ 默认档）
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const ROOT = new URL('../../', import.meta.url);

function read(rel) {
  return fs.readFileSync(new URL(rel, ROOT), 'utf8');
}

// ---- ① `StepKind` 闭集合（值 = 顺序 = 名字，三者一起冻结）-------------------------------
const EXPECTED_KINDS = [
  'AwaitScript=0',
  'AwaitPosition=1',
  'Wait=2',
  'Click=3',
  'ProfileBegin=4',
  'ProfileEnd=5',
  'Quit=6',
  'MouseMove=7',
  'MouseDown=8',
  'MouseUp=9',
];

// ---- ② `stepKindName()` 的线上名字（`Step[]` 的 `kind` 字段 = 这一份；driver 按它 switch）----
const EXPECTED_WIRE_NAMES = {
  AwaitScript: 'await script',
  AwaitPosition: 'await position',
  Wait: 'wait',
  Click: 'click',
  ProfileBegin: 'profile_begin',
  ProfileEnd: 'profile_end',
  Quit: 'quit',
  MouseMove: 'mouse_move',
  MouseDown: 'mouse_down',
  MouseUp: 'mouse_up',
};

// ---- ③ 需求树的 `repro:` 锚（`文件#子串`；子串是**逐字**的指令行）----------------------
const ANCHORS = [
  ['apps/emulator-lift/scripts/recipes/title-loaddata.plan', 'click 1070 480'],
  ['apps/emulator-lift/scripts/recipes/se-eof.plan', 'click 830 592'],
  ['apps/emulator-lift/scripts/recipes/sn0000-save.plan', 'click 650 320'],
  ['apps/emulator-lift/scripts/recipes/sn0000-quit.plan', 'click 650 320'],
  ['apps/emulator-lift/scripts/recipes/sn0000-quit.plan', 'click 1180 640'],
  ['apps/emulator-lift/scripts/recipes/sn0000-quit.plan', 'click 1186 373'],
];

test('recipe-freeze/step-kind-closed-set：`StepKind` 恰好那 10 个值（增 / 删 / 改名都红）', () => {
  const header = read('apps/emulator-lift/src/runtime/recipe.h');
  const match = /enum class StepKind\s*:\s*std::uint8_t\s*\{([\s\S]*?)\n\};/.exec(header);
  assert.ok(match, '找不到 `enum class StepKind : std::uint8_t { … };`（结构被改了？）');
  const body = match[1]
    .split('\n')
    .map((line) => line.replace(/\/\/.*$/, '').trim())  // 去 `//` 注释
    .filter((line) => line.length > 0)
    .join(' ');
  const entries = body
    .split(',')
    .map((piece) => piece.trim())
    .filter((piece) => piece.length > 0);
  // 每一项都必须是**显式编号**（⛔ 不靠隐式递增：编号是 `Step[]` 之外的 ABI 序）。
  for (const entry of entries) {
    assert.match(entry, /^[A-Za-z_]\w*\s*=\s*\d+$/, `StepKind 的成员必须显式编号：${entry}`);
  }
  assert.deepEqual(entries.map((e) => e.replace(/\s+/g, '')), EXPECTED_KINDS);
});

test('recipe-freeze/step-kind-names：`stepKindName()` 的名字 = driver 的 `KIND_*`（两侧同一份字符串）', () => {
  const source = read('apps/emulator-lift/src/runtime/recipe.cpp');
  const match = /const char\* stepKindName\(StepKind k\)\s*\{([\s\S]*?)\n\}/.exec(source);
  assert.ok(match, '找不到 `stepKindName()`（名字源被搬走了？）');
  const seen = {};
  for (const m of match[1].matchAll(/case StepKind::(\w+):\s*return\s*"([^"]*)";/g)) {
    seen[m[1]] = m[2];
  }
  assert.deepEqual(seen, EXPECTED_WIRE_NAMES);

  // `Step[]` 的 JSON **只能**用这一份名字（⛔ 第二份字面量列表）。
  const json = read('apps/emulator-lift/src/runtime/recipe_json.cpp');
  assert.ok(json.includes('stepKindName(s.kind)'), 'recipe_json.cpp 必须用 stepKindName() 产 `kind`');

  // driver 认的名字必须与 C++ 的线上名字**同一组**（⛔ 只改一边 ⇒ 静默对不上）。
  const driver = read('apps/emulator-lift/scripts/plugins/lib/recipe.js');
  const kinds = [...driver.matchAll(/^const KIND_[A-Z_]+ = "([^"]*)";$/gm)].map((m) => m[1]);
  assert.deepEqual(kinds.sort(), Object.values(EXPECTED_WIRE_NAMES).sort());
});

test('recipe-freeze/parser-no-new-instructions：parser 不许长出 `global_*` / 变量 / 新指令', () => {
  const files = [
    read('apps/emulator-lift/src/runtime/recipe.h'),
    read('apps/emulator-lift/src/runtime/recipe.cpp'),
    read('apps/emulator-lift/src/runtime/recipe_json.cpp'),
  ];
  const forbidden = ['global_set', 'global_get', 'set_var', 'get_var', 'StepKind::Global', 'variable_set'];
  for (const text of files) {
    for (const word of forbidden) {
      assert.ok(
        !text.includes(word),
        `冻结面出现了新指令 / 变量：${word}（合同 §5 C3：StepKind / parser 冻结）`,
      );
    }
  }
});

test('recipe-freeze/repro-anchors：5 条 `repro:` 锚行逐字在场（改一个空格就红）', () => {
  for (const [file, anchor] of ANCHORS) {
    const text = read(file);
    const lines = text.split('\n').map((line) => line.replace(/\r$/, ''));
    assert.ok(lines.includes(anchor), `${file} 里没有逐字的锚行：${JSON.stringify(anchor)}`);
  }
  // 锚的**索引**（`scripts/recipes/README.md` 的"这几行是需求树的 `repro:` 锚"那一块）同样逐字。
  const readme = read('apps/emulator-lift/scripts/recipes/README.md');
  for (const anchor of ['click 1070 480', 'click 650 320', 'click 1186 373', 'click 1180 640', 'click 830 592']) {
    assert.ok(readme.includes(`\n${anchor}\n`), `scripts/recipes/README.md 的锚块里没有：${anchor}`);
  }
});
