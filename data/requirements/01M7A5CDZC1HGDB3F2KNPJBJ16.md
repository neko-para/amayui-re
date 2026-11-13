# lift overrides 台账：从「结构同步」升级到「语义可失效」（kind 三值 + scope_ref + 模块集与 AGERC 单列 + 删 12 条不可达）

- id: REQ-01M7A5CDZC1HGDB3F2KNPJBJ16
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, overrides, ledger]

## 背景（2026-11-13 审计，`.tmp/lift-overrides/audit.md`）

`data/lift/overrides.json` 的结构**没有漂移**（`lift sync` dry-run 逐字节一致，sha256 同），腐坏是**语义**的：
- 74 条里 **47 条是 `addStub`（有 fn、静默 no-op）却记成 `kind: stub`**，与 27 条"响亮 trap"混成一个词 ⇒ 用户看到 `GetDC=stub_trap` 便以为"GetDC 没实现"（实际能力由 DDraw surface `GetDC` 承载，且该入口**永远调不到**）。
- **37 条 reason 已不成立**：27 条拿 `M1/M2/M3/plan D4` 当锚；SCOPE §1 的音频/视频/3D 三行本月都改过口径。
- `check` 的核心断言 `e.reason === s.reason` 是**同义反复**；reason 还是用正则从 C++ 源码里读，**不认相邻字面量拼接** ⇒ `kernel32_agerc.cpp` 那处落成 `…进 "\n              "g_nameLenMax…`，check 与同一份错值比所以绿。
- 模块面只扫 AGE.EXE：AGERC.DLL 有 **30 条裸导入**账上完全没有，运行期被 `runtime/shims.cpp` 现场补成通用理由，绕过 SCOPE §3"每条 stub 都要有理由"。

## 已裁决（2026-11-13 用户）

1. **删掉 12 条不可达入口**（C++ 登记 + JSON 一起删）：`user32!GetDC/ReleaseDC/SetFocus/RegisterClassA`、`gdi32!CreateCompatibleDC/CreateDIBSection/BitBlt/GetDeviceCaps`、`kernel32!FindResourceA/SizeofResource/LoadResource/LockResource`。
   `no-bare-gap` 只对导入生效 ⇒ 删它们不产生裸缺口。能力由 DDraw surface `GetDC` / `RegisterClassExA` / `PeResources` / GDI DC 提供。
2. **AGERC 的导入不登记为 gap**：`裸缺口=0` 只对**会执行**的模块（AGE.EXE）强制；AGERC（代码从不执行）的导入只进 `lift report` 的独立计数与清单，不进 overrides 台账。
3. **换机制**：`scope_ref` 必填，reason 从 SCOPE §1 的**具体行**派生；`kind` 三值化。

## 要做

1. **模块集**：`tools/lift.mjs` 声明 `{AGE.EXE: executed=true, corpus/assets/agerc/AGERC.DLL: executed=false}`（路径以仓库内的免壳件为准，⛔ 不用 `install/AGERC.DLL`——那是带壳原件）。
   `never-imported` 改成 `not-referenced`（不在**任一**导入表 ∧ 不是被拦截的导出）；`bare` 只对 executed=true 计；AGERC 的 bare 单列 `bareUnexecuted`。
2. **kind 三值化**：`replace`（add）/ `noop`（addStub：有 fn、静默成功）/ `trap`（addUnimplemented：响亮失败）。派生缺口仍是 `trap`。`describe` / `overrides.md` / 测试同步。
3. **`lift check` 加逐字节断言**：`readText(overrides.json) === renderOverrides(expectedEntries(...))`（唯一写入口 = `lift sync --write`）。
4. **`scope_ref` 机制**：每条 noop/trap 必须有 `scope_ref: "SCOPE.md#<锚点>"`（指向 SCOPE §1 的**具体行/条目**）或 `decision_ref: REQ-…`；`check` 断言：锚点可解析且该行点名该 API（或该 API 落在该行"仍然不做"子句里）；若该行已改成"已实现"而条目还在 ⇒ **红**。
   reason 由该行文本派生（或强制 reason ⊆ 该行文本），⛔ 不再保留自由散文。
   同时修 **reason 读取**：相邻字符串字面量必须拼接（`kernel32_agerc.cpp` 那处）。
5. **`MODULE_REASONS` 退役**：派生缺口不再用一张模块级理由表（它正是过期文本的产地），改成指向 SCOPE 行。
6. **清理 37 条过期 reason**：按 SCOPE 今天的口径重写（音频→已实现方案C / 视频→已实现方案A / 3D→部分落地）；`imm32!ImmAssociateContext` 引用的"IME 不做"条款不存在 ⇒ 要么在 SCOPE §1 立一行，要么删条目。
7. **守卫/测试**：`tools/test/lift.test.mjs` 加：①字节一致 ②scope_ref 悬空 ⇒ 红 ③kind 三值 ④AGERC bare 不进 overrides。变异（`tools/mutate-check.mjs`）：改一条 scope_ref 成不存在 ⇒ 红。
8. **口径文档**：`data/lift/overrides.md` 更新（kind 三值 / scope_ref / 唯一写入口 / AGERC 单列）。

## 判据

- `pnpm tools lift check` 绿；`lift report` 出现 `bareUnexecuted`（AGERC）与 `not-referenced` 计数；
- `overrides.json` 与 `lift sync` 输出逐字节一致；
- 被删的 12 条在 `rg` 里消失（C++ 与 JSON 都没有），且 `lift report` 的导入覆盖仍 100%；
- 未改动引擎行为（只删不可达登记；`ctest` 95 注册 / 91 通过 / 4 SKIP）。

## 未做 / 待裁决

- `CreateProcessA` 做不做；MF 15 条 / MCI+mixer 6 条是否永久 no-op —— 这两组留给用户裁决，本单只按现状登记并保留。
