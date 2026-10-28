# spec 基建：CTest 证据分级（注册位置判 pure/assets）

- id: REQ-01M5Z9RCCT0SK9N3097KQ3CWGX
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/ledger.test.mjs#★ CTest 分级

## 问题

`spec.mjs` 判 `reviewed` 时只认 `ledger` 记录或 `tier=pure` 的守卫证据，而 `tier` 来自测试文件首行的
`@env pure` pragma。`apps/emulator-cxx` 的守卫是 **CTest**（`add_test`），一个 `CMakeLists.txt` 里同时有
免安装与需游戏安装两档测试 ⇒ pragma 挂不上（挂文件头上就成一刀切）。

结果：**行为级、免安装、默认门禁就会跑**的 CTest 证据只能当"接线证明"，一票规则升不到 `reviewed`。

## 做法

判据换成**注册位置**（`tools/lib/ledger.mjs` 的 `cmakeTestTier`）：

- `add_test` 写在**顶层** ⇒ `pure`（默认 `ctest` 就会跑、免游戏安装）；
- 落在任何 `if` / `foreach` / `function` / `macro` / `while` / `block` 里 ⇒ `assets`。

★ 为什么不做"条件里提没提到 `EMULATOR_CXX_INSTALL_DIR`"这种名字启发：这一档要回答的是**默认门禁跑不跑**，
名字启发会把 `if(WIN32)` 里的免安装测试判成 pure，也会漏掉被 `if(TARGET …)` 包住的安装测试。
★ 为什么不用 pragma：CTest 没有"一个用例一个文件"的落点；位置是唯一**不可能说谎**的来源 ——
把 `add_test` 挪进 `if(...)`，分级**自己**就变了。

## 判据

- `tools/test/ledger.test.mjs`：`★ CTest 分级：…（顶层 = pure，落在块里 = assets）`（含 elseif/else 不算新块、注释掉的注册不算注册、名字前缀不许互相吃掉）。
- `tools/test/spec.test.mjs`：装机流程里的 CTest **不能**撑 reviewed、顶层注册的**可以**。
- `tools/mutate-check.mjs` 新增一条变异（把分级退化成"一律 pure"）⇒ 上面那条必须红（已手验）。
- 文档：`AGENTS.md` §5 与 `apps/emulator-cxx/README.md` 的「运行 / 门禁」。

## 效果

配置界面的 5 条规则 + 本轮新增的 3 条（文本槽偏移 / 配音排队 / BGM 淡变）都因此升到 `reviewed`。
