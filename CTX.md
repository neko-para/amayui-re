# CTX · 交接（2026-11-17 上午 · win32 · 两棵工作树）

> 这是**待办 / 裁决清单**，不是口径文档。口径看 `AGENTS.md`（§12 实现 · §14 双树 · §15 派活）·
> `apps/emulator-lift/README.md` · `docs/03-emulator/`。
> **进度真源**仍是 `pnpm tools requirements plan` —— ⛔ 不在这里抄数字（本文件只写"要你决定什么"）。
> 两棵树：`E:\Projects\amayui-re` = `main`（真相：需求单 / 本文件）· `E:\Projects\amayui-re-dev` = `dev`（代码 / 工具）。

---

## A. 一句话现状

**三件事刚收口**（本轮）：① 变异自检的 `buildTarget` 已有**静态存在性检查**并回填全部 CTest 条目；
② SC0000 的**第 1 步渲染接口**（`D3DXLoadSurfaceFromSurface` 整条族）落地；③ AOT 音频 **S0–S3 全落地**
（`--audio sdl --sound` ⇒ `device=ok`、`fills>0`，缺省档 **pump 次数 = 0**）。

读数（win32 / `windows-x64` = RelWithDebInfo / `main` HEAD `f91031b`）：
`ctest` **104/104 · 0 skip** · `pnpm test` **574** · `pnpm verify` **500** ·
`mutate --list` **121/121**（`buildTarget` 97 条与 `add_test` 映射一致）· `requirements validate` **6/6**。
两棵树工作树都干净；`dev` 是 `main` 的祖先（历史没分叉）。

★ 上一版这份文件（2026-11-16 晚 mac→win32）里的 W1「ctest 全量还没在 Windows 跑过」**已不成立**：
Windows 上已多次全量跑过，现在是 104/104、0 skip。

---

## B. 需要你裁决（agent 不能替；每条我给了建议，可以一句话推翻）

1. **`22→32` / `33→21` / `33→32` 要不要一并拒绝？**
   它们与 `22→21` 是**同一个形状**（X8 的顶字节照抄进目标 alpha ⇒ 目标像素变透明）。
   本轮只按点名的 `22→21` 改了（那一条已改成响亮停）。**建议：一并拒绝**，理由是"造信息"这件事没变；
   代价是三条判据表与两条测试要同时改（详情在 `REQ-01M7K04EY6…` 的收口说明里）。

2. **自拷贝的"别名"形态要不要也拒？**
   现在只判**对象身份**（`src_surface == dst_surface` ⇒ `D3DERR_INVALIDCALL`）。
   两个**不同** `IDirect3DSurface9` 包**同一块 arena** 时仍会重叠（同一 UB）。真机行为未取证。
   **建议：不扩**（保持"对象身份"口径），等真机证据。

3. **`boundary` 那 29% 要不要排期？**
   窗口 profile 的结论：`lift` 50% / **`boundary` 28.7–29.9%** / `shim` 17.5–18.6%，且**没有任何单条 shim ≥10%**
   ⇒ `boundary`（块边界宿主税，六件事的合计）是唯一够格的候选。单已立（`REQ-01M7JV8S4MGY87K76YDQ9WRKA8`）。
   你说过"先开单、暂不处理"。**建议：等真撞到卡顿再切**。

4. **音频 S4「听见」的判据用什么形态？**
   `pushed_bytes > 0` 已经作为读数出现，但**没有判据**。两条路：
   ① 人类感官（"我听见了"）—— 只能你做；② assets 档 CTest 的端到端断言（`--audio sdl` 跑 N 帧，断言 `pushed>0` 且 `underruns` 上限）。
   **建议 ②**（可复跑），①只当一次性确认。⚠ **前置**：见 §C-1，游标缺陷不修的话 `pushed` 恒 0。

5. **`buildTarget` 的假绿面：静态检查现在做吗？**
   已发现一类真漏洞：守卫 **spawn 产品 exe** 时，`--target <守卫>` 只重建守卫 exe ⇒ 产品 exe 停在变异前
   ⇒ 变异**根本没施加**，却看起来"守卫没抓住"。已修一例（`cli-aot-flags`），**静态检查还没做**
   （单 `REQ-01M7…`，判据 = 删掉 `add_dependencies` ⇒ `--list` 非 0）。
   **建议：现在做**（它决定"变异自检"这四个字还算不算数）。

6. **SC0000 第 2/3 步什么时候做？**
   第 1 步（D3DX surface 族）已落地，剩下的缺口在 `REQ-01M7JZKNDXZF5FM2WXTAQ319KG` 的 `## 还没做`。
   **建议：排在 §C-1/C-2 之后**（那两条是"已有东西在错"，比"还没有的东西"优先）。

**旧清单里仍活着的三条**（本轮没动，状态已核）：

- `REQ-01M7J97QY66KMP5SBPCTR0S3TP`（open）· **加载屏第三条门 `await global/local`** —— 加载屏与保存屏共用 `SAVE.BIN` 同区间，
  位置门**区分不了**，唯一区分量是 `global a9cb` 位。做它要"按槽号读 global/local 的只读取址原语（含解码）"。
- `REQ-01M7J624JNTFBY7WF52J50KQDC`（open）· **控制层探索手感 7 条**（挑几条做：`--until-pos` > `click` 回执 > 门失败 fail-fast > 缺省 orphan/root）。
- `REQ-01M7J6D62QFRZC2454GV7CKJDJ`（open）· **AOT 覆盖缺口 6 条**（最像"该补"的是 `--dump-frame` 开跑前建目录）。

---

## C. 我建议的下一步顺序（每条都有单 / 判据）

1. **`REQ-01M7JZM1QQ728KX1C0G2F232T1` 的子单（🐞S2）** —— AOT 虚拟音频档的**交付游标用自开机绝对时间**
   ⇒ `played_ ≈ 8e10` ⇒ `generateTo()` 在 `out_frames64 > 0xFFFFFFFF` 处**静默 return** ⇒ `mixed/pushed` 恒 0、
   `--audio-dump` 永远只有 44 B 头。**它是 S4 的真前置**，也是"读数随机器开机时长变"这种不可复现来源。
2. **`buildTarget` 假绿面的静态检查**（§B-5）。
3. **SC0000 第 2/3 步**（§B-6）。
4. 有空再看 `boundary`（§B-3）。

---

## D. 本轮已收口（⛔ 不要重做）

| 东西 | 结论 | 凭据（可复核） |
|---|---|---|
| 变异自检 `buildTarget` | 从 `add_test(NAME … COMMAND …)` 派生 target；填错点名「应为 X 实为 Y」 | `tools/mutate-check.mjs` + `tools/test/mutate-static.test.mjs`（97 条回填） |
| M-lift-3（图形 shim 效率） | 第 1–4 项落地（`Clear` 行级填充 / 日志先判可见性 / 容器复用 / 顶点去重）；**目标 `≤2.5 s` 按用户裁决撤回**（没撞到渲染瓶颈），第 5 项（hash 抽样）**drop** | `data/requirements/01M7FRXZ3K23M0WVTJQ9NNN35B.md` |
| SC0000 第 1 步 | `D3DXLoadSurfaceFromSurface`：半开矩形 / 不隐式裁剪 / 只 1:1 / 无 colorKey / RGBA↔BGRA | `REQ-01M7JZKNDXZF5FM2WXTAQ319KG` · `ctest d3dx9-load-surface` |
| 音频 S0–S3 | 定档谓词 / 参数面 / `begin-end` / 块边界泵 + `fill_bytes` | `REQ-01M7JZM1QQ728KX1C0G2F232T1` · `aot-audio-flags` / `aot-audio-pump` / `aot-audio-wiring` |
| 复审 7 洞 | 全收口（含 `22→21` 改响亮停、自证式断言换成读引擎 `muted=`） | `REQ-01M7K04EY6D3GQNB5DRWP0DYJ8` · commit `7242968` |
| 长程模式可提交 | long-horizon 的"不许 commit"措辞已改（口径仍照 §14.1） | `.agents/skills/long-horizon/SKILL.md` |
| 已废弃/已完的旧账 | `REQ-01M7704NBZEMADBHRZCKPQ7W1S`（搬 POSIX）= **dropped**；`REQ-01M7GA4B8T9JXFDAYK46YHNH7K`（自动到加载存档屏）= **done** | `requirements show <id>` |

---

## E. 坑（本轮新踩的，别再踩）

1. **变异形态必须先能编译**：`/W4 /WX` 下"删掉最后一个实参"会让参数变 unused ⇒ **编不过** ⇒ 工具记「不可判定（构建失败）」
   而不是"守卫红了"。改法：`(void)x;` 或"运行时恒真"的写法（同 §12.7 的 `if (false)` ⇒ C4127 那类）。
2. **`buildTarget` 只重建守卫的 target** ⇒ `out/` 里别的 exe 会停在变异版。工具因此在**一轮结束时全量重建**；
   ⛔ 别把那次重建当浪费删掉（删了之后"拿 `out/` 里的 exe 测 A/B"就是静默读一个被改坏过的二进制）。
3. **单条变异 ≈ 2 分钟**，其中 ~90 s 就是那次收尾全量重建 ⇒ ⛔ 别指望"几十秒"；**全量扫**才是 `buildTarget` 的收益点。
4. **spawn 产品 exe 的守卫要 `add_dependencies(<守卫> <产品 exe>)`**（见 §B-5），否则变异假绿。
5. **AOT 音频泵的粒度下界是"一次派发"**：两次块边界间最长实测 **5900 万条 guest 指令** ⇒ 单个超长派发期间音频完全不推进。
   S4 若欠料先看这条，⛔ 不是加大泵频。
6. **`lastHash` / `insns` / `dispatch` / lookups 每次 run 都会漂**（不是改动引起）：指纹只认
   `exit` + `frames` + `stop @` + arena 三元组 + `table misses`（§12.6）。

---

## F. 恢复命令

```bash
cd apps/emulator-lift
cmake --preset windows-x64 && cmake --build --preset windows-x64
ctest --test-dir out --output-on-failure          # 期望 104/104 · 0 skip
cd ../.. && pnpm test && pnpm verify && node tools/requirements.mjs --validate
node tools/mutate-check.mjs --list                # 121 条静态检查（含 buildTarget 映射）
node tools/mutate-check.mjs --match '<子串>'      # ★ 日常只跑这一条（全量是刻意扫）
```

---

## G. 交接注意

- **本文件不维护**：只在"要换设备 / 要看交接"时重建（`AGENTS.md` §14.6）。平时**别顺手更新它**。
- **进度与"还要做什么"只信 `pnpm tools requirements plan`**；本文件里的单号会过期，`plan` 不会。
- **需求单只在 `main` 改**，且只经 `node tools/requirements.mjs --add/--set … --write`（⛔ 不手改 `data/requirements/*.md`）。
