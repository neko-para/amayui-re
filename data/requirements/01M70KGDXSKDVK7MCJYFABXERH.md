# 🐞S3 变异清单在 HEAD 上就有多条 from 失配 ⇒ 全量 `node tools/mutate-check.mjs` exit 1（日常只用 --match 所以没人发现）

- id: REQ-01M70KGDXSKDVK7MCJYFABXERH
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: tools/test/mutate-static.test.mjs#a) 不变量：真清单 0 条静态失配（每条 file 都在、from 是它的子串；不适用的逐条报出）
- repro: tools/mutate-check.mjs#mutates
- severity: S3
- done_reason: 5 条 from 失配全部重定向/改形态（0 删除），实测各自 --match 变红；根因防线已加：--list 现在做静态存在性检查（81/81 验过、失配即非 0 退出），并加守卫 tools/test/mutate-static.test.mjs（含「构造假失配必被报出」的非恒真证明）。另修 --match 空匹配的假绿。
- tags: [emulator-lift, tooling, mutation]

## 判据（存在性 + 观测）

- 现象：`node tools/mutate-check.mjs`（**全量**）在**本批开工前的工作树**上就 exit 1 —— 不是某条变异"没能让守卫红"，
  而是清单里有若干条目的 `from` 片段在当前源码里**已经不存在**（`--match` 只跑一条，所以日常没人发现）。
- 实测（父级逐条核对，`Select-String -SimpleMatch` 计数）：

| 条目所在文件 | `from` 片段 | 现在还找得到吗 |
|---|---|---|
| `apps/emulator-lift/src/runtime/shims.cpp`（`ctest:shim-abi`，stdcall 弹参） | `call.stdcall_bytes = (def.conv == Conv::Stdcall) ? … argc) * 4u : 0u;` | **0 次** |
| `apps/emulator-lift/src/shims/user32_menu.cpp`（`ctest:shim-abi`，`SetMenu` no-op 返回） | `菜单整体 no-op ⇒ 返回 TRUE（SCOPE §1…` | **0 次** |
| `apps/emulator-lift/src/runtime/image.cpp`（`ctest:pe-loader`，节 raw 越界） | `if (copy && raw_end > img.bytes.size()) {` | **0 次** |
| `apps/emulator-lift/src/runtime/vfs.cpp`（`ctest:file-io`，macOS creation 退回 ctime） | `out->create_100ns = toWindows100ns(nsOf(st.st_birthtimespec));` | 1 次（这条仍有效） |

  另两条（`vfs.cpp` 的 `find-data-times` 三条时间同一、`vfs.cpp` 的 `.lift-tmp` 清理）由实现者报告为同类陈旧 —— 复核时按同样方法逐条验。
- ⇒ 判据：**全量 `node tools/mutate-check.mjs` 必须 exit 0**（每条要么真红、要么被显式裁决为"该变异已无对象"并说明去了哪）。

## 为什么它会静默存在

- 日常只用 `--match <一条>`（父级派活给 subagent 时就是这么要求的）⇒ 单条永远绿，**全量没人跑**。
- 这些 `from` 是从 `runtime/` 早期大文件（`shims.cpp` / `image.cpp` / `vfs.cpp`）里割出去的：文件还在、函数搬去了
  `src/shims/**` 或改了形状 ⇒ `from` 失配。
- 工具对"`from` 找不到"的处理只是让该条无法判定（全量 exit 1），**不影响**别的条目。

## 修法边界（给将来的 agent）

- ⛔ **不许删条目了事**：先逐条判"这条变异守的不变量现在由谁守"——
  ① 代码搬了家 ⇒ 把 `from`/`to` **重定向到现址**（保持"同一处破坏、同一个守卫红"）；
  ② 不变量已被更细的守卫取代 ⇒ 改成那条守卫能抓的破坏形态，并说明理由；
  ③ 真的没有了 ⇒ 才允许删，且必须在条目的 `what` 或提交说明里写"为什么不再需要"。
- 每条改完都要**实测**：施加 ⇒ 对应 CTest 红 ⇒ 还原（工具会自动全文比对 + 重建 + 复跑基线）。
- ⛔ 不要把阈值/断言放宽来"让它能红"。

## 与别的单的关系

- 与本批的 `REQ-01M701NYF2B3YV1YFSZBMPQZ41` 无关（那条单的实现者按父级要求只用 `--match`）。
- 与 `tools/mutate-check.mjs` 的"守卫自检"定位直接相关：**清单自己失配 = 自检失效**（`AGENTS.md` §5 `pnpm test:mutation`）。
