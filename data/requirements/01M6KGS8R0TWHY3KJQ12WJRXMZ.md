# AGERC 装载：用免壳 PE 镜像（ALF 条目 / 安装树根件），拆壳件与伪 HICON 降为应急

- id: REQ-01M6KGS8R0TWHY3KJQ12WJRXMZ
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, m2]

## 一句话

把 AGERC 的装载口径改成"**免壳 PE 镜像**"：`--agerc` → `--game-dir/AGERC.DLL` → 从
`SYS4INI.BIN` + `DATA1.ALF` 切 `AGERC.DLL` 条目；把"拆壳件 / 伪 HICON"从**预期路径**降为**应急**。

## 现状（取证：`repo:apps/emulator-lift/docs/agerc-image.md`）

- `DATA1.ALF` 第 21072 条 = **免壳 Debug build**（sha256 `f48d1c0f…`，849,408 B，7 节、21 导出、
  **40/40 资源可映射**、`FileVersion 39.1.0.0`），即 `corpus/disasm/files/AGERC.DLL.lst` 的**真前身**
  （`.lst` 头 `Input SHA256` 与它逐字节相同）。
- 测试安装树根件（sha256 `6241de66…`，848,896 B）是它的**汉化派生**：`.text` 逐字节相同；
  10 条 `RT_MENU`/`RT_DIALOG` 被替换；**图标与版本资源逐字节相同** ⇒ M2 的图标/版本两条路不可区分。
- 只有正版安装树根件（sha256 `5f3189…`，335,872 B，AsProtect）资源不可读（**26/40** 条无文件字节；
  另 14 条能映射，其中 13 条字节不是真资源，`RT_MANIFEST` 恰好相同）⇒ 需要"回退到 ALF 条目"的分支。
- 引擎是**按名字** `LoadLibraryA("AGERC.DLL")`（`.lst` EA `0x48E74B`；`"AGERC.DLL"` 全语料仅 3 处代码引用）
  ⇒ 真机加载的是**应用目录**那份；"从 ALF 装载"是**模拟器的替代策略**，不是引擎行为。

## 要做什么

1. **新增** `apps/emulator-lift/src/runtime/alf_index.{h,cpp}`：
   解析 `S4IC`/`S5IC` 索引头 + LZSS 段 + TOC；`findEntry(name)`；`readPayload(dir, entry)` 按
   `(archiveIndex, offset, length)` **按需**读（⛔ 不整份读 1 GB；⛔ 不搬 `apps/emulator-cxx/src/core/alf.*` 的 core）。
2. `repo:apps/emulator-lift/src/main.cpp`（`main()` 的 AGERC 段）：三步来源（显式 `--agerc` → 根件 → ALF 条目），
   把**来源标签**（`root` / `alf:SYS4INI.BIN#21072`）与 sha256 传给 `Machine::loadAgercModule` 并打日志。
3. `repo:apps/emulator-lift/src/runtime/machine.cpp` `Machine::loadAgercModule`：接受 `(bytes, origin_label)`；
   保持 `agerc_base = 真实 ImageBase`、`cpu.shim.agerc_image` 语义不变。
4. `repo:apps/emulator-lift/src/shims/user32_resource.cpp` `LoadIconA`：正常路径从镜像的 `RT_GROUP_ICON`
   （`IDI_ICON1`）→ `RT_ICON`（id 3–17，lang 1041）拼 `HICON`；伪 HICON 只在 `.rsrc` 数据项映射失败时用，
   且必须 loud log。
5. `repo:apps/emulator-lift/src/shims/kernel32_version.cpp`：逻辑不改（已优先 `agerc_image`）；加启动日志打
   `FileVersion`/`ProductVersion`，并保证 `LoadLibraryA` 与 `GetFileVersionInfo*` 指向**同一份字节**。

## 判据

- 从 `links/install` 切出的 `AGERC.DLL` 条目 sha256 = `f48d1c0f…`；`RT_GROUP_ICON/IDI_ICON1`（216 B）=
  `a49cb23c…`；安装树根件 = `6241de66…`，图标 sha 相同（**相同是预期**）。
- 正版树（`links/raw`）走回退路径后 `LoadLibraryA("AGERC.DLL")` 成功、`LoadIconA` 返回非空；
  只有"根件不可用且索引/条目缺失"才 0 + 126。
- 不写 `--game-dir`；`--agerc` 显式给错仍拒绝启动（现状保留）。

## 不做

执行 AGERC 的对话框代码（`SCOPE.md` §1 的模态 no-op stub 不变）；改 `apps/emulator-cxx`；
为"零售版镜像"另开分支（需要时用 `--agerc` 指 `links/raw/AGERC.DLL__dumped.DLL` 即可，但它与 `.lst`
不是同一 build，⛔ 不能拿它的 EA 引 `.lst`）。
