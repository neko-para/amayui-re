# 🐞 corpus 写入口要求 9 条断言**全绿** ⇒ 缺件的机器上 --add/--set --write 永远失败（本机两次实测回滚）

- id: REQ-01M52F9K25YNPJEXQ74QE4CT9Z
- type: bug
- status: open
- parent: REQ-01M4FEFA9RRP5TKVWH351NV1RD
- order: 10
- repro: tools/lib/manifest.mjs#if (pre.failures > 0) return { ok: false
- severity: S2
- tags: [corpus, manifest, tooling]

## 现场（本机 macOS）

给 `corpus/assets.json` 加一条**完全合法**的 entry（`dest` 与 `origin` 都在盘上、无 sha256、kind/role 合法）：
```
pnpm tools corpus add '{"id":"disasm/c-vocabulary", …}' --write
```
⇒ **回滚**、退出码 1：
```
--add 失败：写后复验未通过（已回滚）：
  - #3 存在性：… 3 处缺失
  - #4 校验和纪律：… 1 处违规
```
`--write --no-hash` 只去掉 #4，**#3 仍挡**；两次都**没有**改动清单（回滚是对的，缺的是"能不能写"）。

## 挡住它的两条，与本条目**无关**

* **#3** 缺的是 `binary/age-original`（`gameInstall:AGE.EXE__dumped.EXE`）与
  `binary/age-sectfix`（`oldRepo:raw-parts/AGE.EXE__dumped.sectfix.EXE`）—— 两者是 **Windows 侧**的文件；
  本机实测整个旧仓（含 `raw-parts/`、`.staging/`）**找不到**它们，`--no-hash` 也救不了。
* **#4** 是 `binary/agerc-modified-install` 的 pin（`6241de…`）与**只读**旧仓 `install/AGERC.DLL`（`5f3189…`）不符 ——
  已由 `REQ-01M4GE4HSJGZ4XF9BSTZFHKXKG`（dropped）裁决为**跨机器 root 解析差异**，真问题在父单。

⇒ 结论：**写入口的预验是"零失败"而不是"不新增失败"**（`tools/lib/manifest.mjs` 的 `if (pre.failures > 0) return { ok: false …`），
于是在任何"缺件"的机器上，**任何**条目都加不了、也改不了（翻牌 `deferred → lfs` 同样写不了）。

## 判据（修法方向，二选一）

1. 预验改成 **"不新增失败"**：对比写前/写后的失败集合，与本条目无关的既有红灯不再挡写；
2. 或给"本机不可见的 origin"一个**显式处置**（entry 级，如 `originUnavailable: 'platform'`），使 #3 只对"本机应可见"的来源报红。

★ 两条都**不是放宽守卫**：新写入的条目自身仍不许引入失败（判据 1 就是拿这个当准）。

## 边界

* win32 侧据 dropped 单的实测是**全绿**的 ⇒ 本单影响面 = **macOS（兼容性机器）与任何缺件的机器**；
  win32 的行为本机无法验证，只有 mac 侧是直接观察。
* 绕道：换一台绿的机器跑 `corpus add`（这也是本单先落在父单名下的原因 —— 与"origin 与机器绑定"同一根）。

## 待落地的命令（修好后、或换一台绿的机器）

`corpus/disasm/defs.h` 已就位（LF 已按仓规统一），条目只差这一步：
```bash
pnpm tools corpus add '{"id":"disasm/c-vocabulary","kind":"tooling","role":"reference","storage":"git","origin":[{"root":"oldRepo","path":"engine/defs.h"}],"dest":"corpus/disasm/defs.h","readOnly":true,"consume":"只读：Hex-Rays 固定词表（宽度别名 + 折叠内在函数）。`pnpm tools disasm-fn shape` 用它定宽度闭集合。","note":"IDA/Hex-Rays 8.3 随包 defs.h。"}' --write
```
（本机 dry-run 已证明该 entry **自身合法**：唯一挡住它的是上面那两条与本条目无关的红灯。）
