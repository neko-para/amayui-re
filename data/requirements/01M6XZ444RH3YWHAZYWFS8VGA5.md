# 复核并迁移 spec/台账里指向废弃实现测试的证据锚（之后才能删那 75 个测试文件）

- id: REQ-01M6XZ444RH3YWHAZYWFS8VGA5
- type: spike
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q

## 为什么有这张单（实测，2026-11）

原计划是"删掉 `apps/emulator` / `apps/emulator-new` 的测试套件，用一条哈希校验替代"。实测把前提推翻了 ⇒ 本轮**暂停删除**，先把证据锚迁走。

### 1. 位置纠正：测试不在那两棵 app 树里

`apps/emulator/**`、`apps/emulator-new/**` 下**没有任何 `*.test.mjs`**（只有实现）。那批测试在 **`tools/test/`**：

- `emulator-new-*.test.mjs`：**60** 个；
- `emulator-*.{test,assets.test}.mjs`：**15** 个（`emulator-address-space` / `emulator-engine-scalars`(+`.assets`) / `emulator-file-config` / `emulator-frame-tables.assets` / `emulator-frames` / `emulator-headless-logo.assets` / `emulator-host` / `emulator-iterate` / `emulator-model` / `emulator-numeric-ops` / `emulator-numeric-purity` / `emulator-pool-regions` / `emulator-snapshot` / `emulator-state-partition`）；
- 合计 **75** 个。
- ★ **不在删除范围**的 5 个混合文件（同时引用 `apps/emulator-cxx` / `apps/emulator-lift` / 语料清单）：`emulator-cxx-audio-slots` / `emulator-cxx-pl-mpeg-sync` / `spec-rule-refs` / `ledger` / `manifest-local`。

### 2. 断链清单（删这 75 个会红的地方；数字来自实测）

| 台账 | 指向它们的引用 | 删后的红 |
|---|---|---|
| `data/spec/bindings.json` | **140 条 `guard:` 证据**（跨 33 个文件） | `pnpm tools spec validate` → **190 个 #7 错** |
| `data/requirements/*.md` | **94 处 `verify` / `repro`**（跨 29 个文件） | `requirements validate` → **35 条错**（路径棘轮 #3） |
| `data/ledger/*` | 写入闸门的 `guard:` 锚 | `ledger.test` 的"写入闸门 / 锚的元判据"红 |

### 3. 收益 / 代价（这就是本轮不删的理由）

- 删后 `pnpm test` 从 **1159 例 / 25.2 s** 变成 **507 例 / 22.5 s** ⇒ 省 **~3 s**；
  75 个文件占 **52% 的用例数**，却只占 **~1.2 s 的耗时**（其余是无关文件的时间）。
- 代价：三份台账 140 + 94 处证据断链；`reviewed` 规则的强证据被打空（可能要降 `draft`）。
- ⇒ **用例数不是目标，绿的门禁才是**。所以先迁证据、后删测试。

## 怎么复核（命令都能重跑）

```bash
node tools/test-run.mjs --list                 # 分级文件集合（121 pure / 62 assets / 6 external）
pnpm test                                      # 删前基线：1159 例（1158 pass / 1 fail）/ 25.2 s
rg -l "apps/emulator-new" tools/test -g "*.test.mjs" | wc -l          # 60
rg -o "guard:tools/test/emulator-[^\"#]+" data/spec/bindings.json | wc -l
rg -l "emulator-(new-)?[a-z0-9.-]+\.test\.mjs" data/requirements | wc -l
```

## 完成判据（缺一条不算完）

1. 受影响的 spec 规则逐条**重新挂到在维护的证据**（优先 `apps/emulator-lift` 的**顶层 CTest**、`@env pure`），或按裁决降级并在残差里登记"缺哪条观察"；
2. `pnpm tools spec validate` / `pnpm tools requirements validate` / ledger 闸门 **全绿**；
3. 之后才删那 75 个文件，并用一条**纯文本哈希清单**替代"防误改"职责：`node tools/frozen.mjs --write` 生成一行一条 `sha256  <相对路径>`（排序），旁边同名 `.md` 带 `## 怎么查` / `## 怎么改`；守卫 `@env pure`、秒级、不依赖构建 / 网络；
4. `pnpm test` 全绿，且用例数下降**有记录**（删前 / 删后 tests / pass / fail / 耗时各一次）。

## 明确不做

- ⛔ 不许为了让门禁变绿去给 spec 补**弱证据**（assets / external 档不能撑 `reviewed`）；
- ⛔ 不许先降级、之后再把 `reviewed` 标回去；
- ⛔ 不许把需求单 `verify` 改指一个不存在的文件来"先绿再说"；
- ⛔ 不要为了腾预算去删别的单。

## 本轮新发现的暂缓清单（2026-11，`pnpm verify` 落地后补记）

`pnpm test` 改跑合同档（`@suite contract`）之后，下面两类**仍留在门禁里**，但**证据锚身份没变** ⇒ 迁移 / 删除继续暂缓。⚠ 数字与名单**必须重跑**，不要手抄：两种口径差在"算不算正文里顺带提到的路径" —— 父级口径 **33 个** JS 文件；重跑得**字段口径 29**（`bindings.json` 的 `guard:` + 需求单的 `verify:` / `repro:` / `evidence:`）与**路径口径 38**（`bindings.json` + 需求单正文任何位置的测试文件路径，含带 `repo:` 前缀的写法）：

```bash
# 路径口径（字段口径 = 同一份输出，但只取 guard:/verify:/repro:/evidence: 行上的）
rg --no-filename -o '(tools|packages|plugins)/[A-Za-z0-9_./-]+\.test\.mjs' data/spec/bindings.json data/requirements | sort -u
pnpm test:list | rg '·toolchain'     # 与上面取交集 = 工具链档里被台账引用过的那些
```

**六条 lift CTest**（卡在 `repo:` 前缀或 `verify:`）：`heap-layout` ← `repo:apps/emulator-lift/tests/heap_layout.cpp` · `input-tick` ← `repo:apps/emulator-lift/tests/input_tick.cpp` · `profile-window` ← `repo:apps/emulator-lift/tests/profile_window.cpp` · `gdi-font-metrics-cache` ← `verify: apps/emulator-lift/tests/gdi_font_metrics_cache.cpp#cache/size-change` · `audio-device` ← `verify: apps/emulator-lift/tests/audio.cpp#audio-device` · `log-policy` ← `verify: apps/emulator-lift/tests/log_policy.cpp#log-policy`。

★ **名字与路径两种写法都要搜**：上一轮只按测试名搜，漏掉 `repo:apps/emulator-lift/tests/input_tick.cpp` 这类引用（`audio-device` 甚至落在 `audio.cpp` 里，与 CTest 名毫无字面关系），`pnpm tools requirements validate` 当场红 5 条（#6 路径前缀）。`repo:` 指**源码 / 测试文件路径**、`verify:` 指"用例文件 + `#子用例名`" —— 两套写法都得覆盖。

## 失去守卫的两处（2026-11 补记：变异清单 516 → 49 之后）

1. **4 条 Win32 no-op 合同现在 0 守卫** —— `joystick` / `RegQueryValue` / `mciSendStringA` / `MFCreate*`：守卫（CTest `noop`，源 `apps/emulator-lift/tests/noop_shims.cpp`）随本轮裁剪删除；实测 `rg -i 'joystick|RegQueryValue|mciSendStringA|MFCreate' apps/emulator-lift/tests` = **0 行**、`rg -n noop_shims .` = **0 行**。★ 口径纠正：**实现仍在** `src/shims/*`（同一条 rg 换到 `apps/emulator-lift/src` = **55 行** / 6 个文件）⇒ 准确说法是「**语义还在、判据没了**」，不是「语义随文件删掉、全仓 0 命中」。**父级裁决（2026-11-09）：不补守卫，接受风险** —— no-op 是我们**自己的实现选择**（SCOPE §1 的延期决定），不是文档定义的对外合同；guest 只要求「拿到失败码、继续跑」。⇒ 登记为「**已接受的风险**」，⛔ 不是「待补」；触发重开的条件：guest 将来出现**依赖这些具体形态**（假句柄值 / 具体错误码来源）的行为 = 新证据出现。
2. **`apps/emulator-cxx` 的 99 条变异条目已删**（审计判 DROP-L3；HEAD 516 条 → 现值 49 条；这 99 条里 56 条的守卫是 cxx 的 CTest）：**守卫本身还在**（`apps/emulator-cxx/CMakeLists.txt` 里 87 条 `add_test`），但 `data/spec/bindings.json` 有 **101 处**证据锚（去重 **45** 个 CTest 名）指向它们，`repo-guards.md` §T-9 也仍把这批 CTest 记作 spec 证据锚 ⇒ 今天"这些 CTest 会红"**只剩历史声称、没有机械证明**（HEAD 也只覆盖过 45 个锚里的 32 个）。
⇒ **已拆出子单** `REQ-01M6Y28WF82JTAPQMDJN01F8CT`（cxx 证据锚：补代表性变异 / 在 spec 侧降级，二选一 + 判据都在子单里）；本单不再承载这一范围。
复核：`node -e "import('./tools/mutate-check.mjs').then(m=>console.log(m.MUTATIONS.filter(x=>x.file.startsWith('apps/emulator-cxx/')).length))"` = 0 · `rg -o 'apps/emulator-cxx/CMakeLists\.txt#[A-Za-z0-9_-]+' data/spec/bindings.json | wc -l` = 101 · `rg -c '^add_test\(NAME' apps/emulator-cxx/CMakeLists.txt` = 87。
