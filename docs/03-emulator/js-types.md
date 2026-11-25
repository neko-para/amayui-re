# 模拟器插件的 JS 类型面：继续 JSDoc，不引入构建步骤（2026-11-25 裁决）

面 = `apps/emulator-lift/scripts/{plugins,ui}/**`（宿主 QuickJS 直接读的**外置 .js**；类型真源是
`plugins/plugin.d.ts` / `ui/ui.d.ts`）。裁决 = **C（混合）**：契约留 `.d.ts`，运行时仍 `.js` + JSDoc，**不引入构建 / 转译步骤**，产物**不入库**（源码即产物）。

## 怎么查

```bash
node node_modules/typescript/bin/tsc -p apps/emulator-lift/scripts/tsconfig.json --noEmit
node node_modules/typescript/bin/tsc -p apps/emulator-lift/scripts/plugins/examples/tsconfig.json --noEmit
pnpm typecheck   # 最后两条 = 上面两条（examples 2026-11-25 起进门禁）
```

读数（win32 / tsc 5.9.3 / dev `c1c4c4e` 起）：**21 → 16**（`lib/settings.js` 抽走 config 读取面）**→ 3**
（load-save / save10 的谓词与 `@param`）**→ 0**（本轮：`lib/save.js` 的 3×TS7006 补 `@param`）。分布 = field 5 · lib/save 3 · load-save 8 · save10 5；
`scripts/` 下 `rg 'ts-ignore|ts-expect-error'` 零命中。

## ① 21 条的根因归并（不是 21 个独立毛病）

| 类 | 数 | 根因 | 修法（纯注释） |
|---|---|---|---|
| A TS18047 | 10 | `hasConfig()` 回**裸 boolean** ⇒ `if (hasConfig(d,k)) d.get(k)` 不成立（三份同源 helper） | 谓词 `@returns {d is ConfigDomain}` |
| B TS7006 | 7 | 有散文注释、**没有 `@param`**（`pad2` / `isAsciiLetter` / 闭包 `push(item)` …） | 补 `@param` |
| C TS7034/7005 | 2 | `const out = []` 在嵌套 `push` 里被写 ⇒ 元素类型无法判定 | `/** @type {string[]} */` |
| D TS2739 | 2 | 缓存变量标成 `object \| null` ⇒ 链形状被抹成 `{}` | 标真形状 `Chain \| null` |

★ 收益在归并：A 只修**一处**（`lib/settings.js` 的 `openConfigDomain` + 调用点 `d === null || !d.has(k)`）
10 条一起消；B/C/D 各只改注释。⛔ 一条都不许 `@ts-ignore` / 改宽类型 / 删检查。

## ② JSDoc **能**收窄 vs **不能**表达（实测）

能（同一份 tsc + 真 `plugin.d.ts` 副本探针，exit 0）：**谓词**
`@returns {d is ConfigDomain}`（`if` 与 `!… return` 两支都收窄，**跨 import 有效** —— A 的修法）·
断言函数 `@returns {asserts d is ConfigDomain}` · 判别联合 `{has:true,domain}|{has:false}`（收窄成立，
但要改运行时形状 ⇒ 不如谓词）· `@overload` · `@satisfies` · `@template`（含默认参数与约束）；D 类把变量标真形状即可。

不能（语言级缺口；前三条换 TS 才有，第四条 TS 也没有）：
1. **属性路径断言** `@returns {asserts o.cfg is Domain}` ⇒ TS1005（TS 里也非法 TS1228）⇒ 跨函数不变量
   （"调完 `ensureConfig(ctx)` ⇒ `ctx.config` 非空"）只能先取局部再断言。
2. **`as const`** ⇒ `/** @type {const} */` 是 TS2304；字面量联合要手写 `Readonly<{…}>`。
3. **调用点类型实参**：`.js` 里的 `ctx.ui.get<HudState>("hud")` 被当比较运算（TS2365/TS2693）⇒ 只能强转。
4. **运行期时序**（`ctx.now` 只本回调有效 / `ctx.config` 只首拍读 / 页号门只在 SAVE 屏）——不是类型。
5. 控制流坑（实测）：`if (failOnTimeout(…)) return [];` 收不窄 —— 返回值是 `boolean` 而非字面量 `true`，
   TS 把函数尾算作 `undefined` ⇒ TS2322。⇒ 写显式两行；返回值只留给内联表达式。

## ③ 为什么 **不** 做 TS + 构建步骤

1. **会废掉"改 `hud.js` 不用重编"**：`--overlay` / `--plugin` 读的是**源码路径**上的外置 `.js`
   （`CMakeLists` 把源码树绝对路径编进 runtime；`hud.js` `import './kit.js'`）—— 该属性只在"源码即产物"时成立；要 build 的话改一行 HUD 文案就得重编。
2. **会把 Node 变成 ctest 的前置**：CTest 今天零 Node 调用；CMake custom target = 在 C++ 门禁前插
   Node/pnpm 依赖（与 AGENTS §3「无构建步骤」冲突）。pnpm 脚本那条更糟：ctest 前没人保证产物是新的
   ⇒ 改 `.ts` 忘 build 就**静默**用旧 `.js`。
3. **≥8 条 CTest + 4 个 `tools/` 守卫钉死源码 `.js` 路径**：`ui-hud-script` · `ui-kit-script` ·
   `plugin-runtime` · `recipe-driver-parity` · `plugin-headless` · `aot-stress-fellow-switch` ·
   `aot-overlay` · `cli-aot-flags`（后两条逐字断言 `scripts/ui/hud.js` 与 plugins 候选）；
   `plugin-loadsave-page` / `script-map` / `recipe-freeze` / `mutate-check`（8 条变异直接改这些 `.js`）。
4. **产物入库 = 双真源**：`.ts` 与 `.js` 同进 git ⇒ diff 噪声 + "改了哪个"没人分得清。

## ④ 翻面条件（满足**任一**才重审）

- "JSDoc 表达不了"的 4 类开始**挡住功能**（不是手感），且为绕开它写**第二份真相**（复制 / 强转 /
  注释约定）**> 3 处**；或
- 同一契约的漂移在门禁里**红过 2 次**（例：`plugin.d.ts` 样例 vs 实现的 `.BIN` 精确相等）。

⇒ 那时才把 `scripts/plugins/lib/**` 与 `scripts/ui/kit.js` 迁 TS，**产物 `.js` 入库**（保住"运行时读
外置 `.js`、改完只重跑"），**仍然不引入构建步骤**。

## ⑤ 本轮顺带落地

`lib/settings.js`（取配置唯一入口，带谓词）· `lib/gates.js`（`ctx.trigger.*` 薄封装 + 失败收口）· `examples/tsconfig.json` 进门禁（"只靠 `d.ts` 写插件"的唯一可执行样本）· `plugin.d.ts` 用法段订正为 `"TITLE.BIN"` / `"SBUNKI.BIN"`（裸名永不命中 ⇒ 30 s 超时）。
