# 🐞 corpus 写入口要求 9 条断言**全绿** ⇒ 缺件的机器上 --add/--set --write 永远失败（本机两次实测回滚）

- id: REQ-01M52F9K25YNPJEXQ74QE4CT9Z
- type: bug
- status: done
- parent: REQ-01M4FEFA9RRP5TKVWH351NV1RD
- order: 10
- repro: tools/test/corpus-manifest.test.mjs#★ 写入口：这台机器本来就有的红灯**不挡**写入
- severity: S2
- tags: [corpus, manifest, tooling]

## 现场（原始报告，macOS）

给 `corpus/assets.json` 加一条**完全合法**的 entry（`dest`/`origin` 都在盘上、无 sha256、kind/role 合法）⇒ **回滚**、退出码 1：

```
--add 失败：写后复验未通过（已回滚）：
  - #3 存在性：… 3 处缺失      # binary/age-original、binary/age-sectfix、reference/design-discussion
  - #4 校验和纪律：… 1 处违规  # binary/agerc-modified-install：清单 6241de66… / 实际 5f318955… → install/AGERC.DLL
```

`--no-hash` 只去掉 #4，**#3 仍挡**；两次都没改动清单（回滚是对的，缺的是"能不能写"）。
★ 挡它的两条与本条目**无关**：#4 那条已由 `REQ-01M4GE4HSJGZ4XF9BSTZFHKXKG`（dropped）裁决为**跨机器 root 解析差异**，真问题在母单。

## 复核（2026-10，本机 win32，真代码复现）

* 本机 `corpus validate` = 8 通过 / 0 失败 / 1 警告（只有 staging 暂缺）；探针 `.tmp/req-verify/probe.mjs` 造"类 mac"的树（`oldRepo` 无 `install/` `raw-parts/`、`install/AGERC.DLL` = 带壳原版）⇒ #3/#4 复现出与报告**逐字相同**的那两行。
* ★ **只改条目字段清不掉红**（同一棵树上逐条实测）：①「去掉 sha256」⇒ 文件在盘上时 #4 变成"必须写 sha256"（**红从 mac 挪到 win**）；②「改成 `deferred`」⇒ #3 仍点名它（`#3` 的唯一豁免是 `root === 'staging'`，与 `storage` 无关）⇒ 修法只能落在**检查层**。
* 三条 origin 都是**机器本地产物**：`AGE.EXE__dumped.EXE` 是本机 dump 产物（与 `install/AGE_free.EXE` 逐字节相同）；`raw-parts/`、`install/` 被旧仓 `.gitignore:3`/`:1` 忽略且 `git log --all` 为空（**从未跟踪**）。第三处 `DESIGN.md` 是旧仓 HEAD `e90722fe`（2026-09-30）才加的 ⇒ **克隆过期**（另一类）。

## 落地（2026-10）

1. **闸门改成"不新增失败"**：`tools/lib/manifest.mjs` 的 `saveManifest` 比写前/写后的失败**明细集合**（新增 `addedFailures`）—— ❌ 不比检查计数（#3 的 message 是「N 处缺失」这种计数 ⇒ 只比计数会在"+1 处又 −1 处"时**假绿**）。CLI 的 `--add` dry-run 也改成只报"这次新增引出来的失败"。
2. **`origin[].disposition = "machine-local"`**（闭集合 `DISPOSITIONS`）：#3 缺件只 warn **且逐条报数**、#4 不要求也不比对 pin；**入库件不许标它**、**它与 `sha256` 不许共存**；`--scan` 跳过它。
3. **三条 entry 各自落地**：`binary/age-original` · `binary/age-sectfix` 标 machine-local、去掉 pin（字节判据 = `docs/02-engine/README.md` 的 MD5）；`binary/agerc-modified-install` 的 origin 改指 `oldRepo:patch/AGERC.DLL`（**跟踪且走 LFS**、同字节 `6241de66…`）⇒ 保留一个跨机器可校验的 pin（`tools/test/agerc-artifact.test.mjs` 的基准仍是它）。
4. **被本单挡住的那条条目已补**：`disasm/c-vocabulary`（`oldRepo:engine/defs.h` → `corpus/disasm/defs.h`）。

## 凭据与边界

* 守卫 = `tools/test/corpus-manifest.test.mjs#★ 写入口：这台机器本来就有的红灯**不挡**写入`（同文件另有"新引入的失败照样挡"与"明细集合粒度"两条）；`tools/mutate-check.mjs` 有两条对应变异（差集恒空 / machine-local 档失效），实测都会让该守卫红。
* 边界：mac 侧仍**无直接观察**（本机是 win32）；旧克隆上 `DESIGN.md` 那条红**仍会红**（真实且可处置：`git pull`），但它**不再掐死写入**。
