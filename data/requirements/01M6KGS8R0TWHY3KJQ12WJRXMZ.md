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
5. `kernel32_version.cpp`：`VerQueryValueA` 的**字符串**必须交单字节 ANSI（2026-11-05 修，见文末补记）。

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

## 2026-11-05 补记：RCVersion 检查（boot 最后一个卡点）已越过

**期望值来源（EA）**：`0x4BAF26` 的两侧 = `Reg::get("set:RCVersion")` 与 `[RCDLL+0x14]`。期望值来自
`Engine+697620` 的 `Reg`（vtable `.data:0x5297DC`，slot+4 = `sub_4904D0`）；ctor `sub_491880` 给
`set:RCVersion` 的**默认值是 0**（`0x4BAF24` 的 `jz` 因此**跳过整个检查**）；**非 0 值只有一个来源**：
初始化文件 `SYS4INI.BIN`（`_WinMain` `0x4BAB1A` → `sub_414AC0`；`S4IC` ⇒ 解 1,692,004 B）配置段的
`RCVERSION` 键（`sub_494220` 的 `_stricmp(aRcversion)` → `0x494EC0` `atoi` → `Reg::set`）；**不是**注册表、**不是** ALF 里的 AGERC 条目（完整表见 `repo:apps/emulator-lift/docs/agerc-image.md` §5.1）。

**差的是我们**：修复前实测 `[RCDLL+0x14] = 3`、期望 `39`；根因 = `VerQueryValueA` 把块里的 UTF-16LE
指针交回去（引擎 `strncpy_s` 只拷到第一个 `00` ⇒ `atoi("3")=3`），而真机 A 变体的字符串是**单字节 ANSI**、
`puLen` **含结尾 NUL**。改法：字符串子块用 `wideToCp932` 转 ANSI 写进固定暂存页 `0x7FFF0000`、
`puLen = 字节数+1`；`lpData` 仍保留资源原字节（= W 变体形态，差异写进注释）。**没动** `advapi32` / SCOPE 口径。

**守卫**：`repo:apps/emulator-lift/tests/agerc_module.cpp` 新用例 `verqueryvaluea-string-is-ansi`
（`GetFileVersionInfoSizeA` → `GetFileVersionInfoA` → `VerQueryValueA` 全链；断言 `puLen==9`、
`rd8(ptr+1)=='9'`、`atoi` 口径读到 39）。变异：把转换换回"交 UTF-16 指针" ⇒ 该用例 **6 条红**（含 `atoi` 读到 3）。

**boot（干净 `--appdata`）**：`MessageBoxA` 归零；`stop: unimplemented @ 7FFE00AC`（`d3d9!Direct3DCreate9`，
SCOPE §1 的 M3 缺口），`insns = 102,919,944`（两次一致）。已越过 `sub_4BA890`：`--until ea 0x4BAF3C` 命中
⇒ `0x4BAF2C` 的 `jz` 真的跳了。下一站 = `sub_406730`（显示初始化）→ `sub_498CC0`（D3D9 设备创建，
调用点 `0x498D0F`）；DDraw 分支 `sub_43AE30` 未走，因为 `sub_406730` 只在 `set:CreateObject & 1` 时走它。

**遗留（新发现，不在本单）**：① `--appdata` 里残留 `$$AGE.TMP` ⇒ 引擎改走 exe 版本自检 / `_.exe /FIX`
路径（insns 102,962,464 + 多一个 MessageBox）；② 那条路径上打出**乱码 guest 路径**
`E:\Games\Eushully\螟ｩ邨申AGE.EXE`（CP932/UTF-8 混用）。⇒ 建议另开单给「文件与路径」族：boot 基线必须
**从干净的 `--appdata` 起跑**；`GetModuleFileNameA`/VFS 的日文路径要逐字节对。
