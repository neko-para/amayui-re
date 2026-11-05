# AGERC 装载：用安装目录的免壳 PE 镜像（--game-dir/AGERC.DLL），拆壳件与伪 HICON 降为应急

- id: REQ-01M6KGS8R0TWHY3KJQ12WJRXMZ
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- done_reason: 用户 2026-11-05 裁决：AGERC 只从 --game-dir/AGERC.DLL 取（引擎本身也不回退），ALF 条目不作为装载来源、只留作取证对照；实现与文档已按两段式收敛（--agerc 显式覆盖 → --game-dir/AGERC.DLL，拿不到 ⇒ LoadLibraryA 0 + 126）。原计划的 ALF 回退（runtime/alf_index + loadAgercModule(bytes, origin_label)）取消。
- tags: [emulator-lift, m2]

## 一句话

AGERC 的装载口径收敛为**两段**（用户 2026-11-05 裁决）：`--agerc`（显式覆盖）→ `--game-dir/AGERC.DLL`；
**拿不到 ⇒ `LoadLibraryA` 返回 0 + 126**。⛔ 不回退 `DATA1.ALF` 条目（引擎本身也不回退）；
ALF 条目不作为装载来源，只留作取证对照。

## 现状（取证：`repo:apps/emulator-lift/docs/agerc-image.md`）

- `DATA1.ALF` 第 21072 条 = **免壳 Debug build**（sha256 `f48d1c0f…`，849,408 B，7 节、21 导出、
  **40/40 资源可映射**、`FileVersion 39.1.0.0`），即 `corpus/disasm/files/AGERC.DLL.lst` 的**真前身**
  （`.lst` 头 `Input SHA256` 与它逐字节相同）。
- 测试安装树根件（sha256 `6241de66…`，848,896 B）是它的**汉化派生**：`.text` 逐字节相同；
  10 条 `RT_MENU`/`RT_DIALOG` 被替换；**图标与版本资源逐字节相同** ⇒ M2 的图标/版本两条路不可区分。
- 只有正版安装树根件（sha256 `5f3189…`，335,872 B，AsProtect）资源不可读（**26/40** 条无文件字节；
  另 14 条能映射，其中 13 条字节不是真资源，`RT_MANIFEST` 恰好相同）⇒ 正版树默认路径拿不到可映射的 PE
  ⇒ 停在 0 + 126（要跑就显式 `--agerc` 指一份免壳件）。
- 引擎是**按名字** `LoadLibraryA("AGERC.DLL")`（`.lst` EA `0x48E74B`；`"AGERC.DLL"` 全语料仅 3 处代码引用）
  ⇒ 真机加载的是**应用目录**那份；"从 ALF 装载"是**模拟器的替代策略**，不是引擎行为，且按裁决不做。

## 做成什么（两段，已落地）

1. `repo:apps/emulator-lift/src/main.cpp`：显式 `--agerc` → `--game-dir/AGERC.DLL`（VFS 解析）；
   都没有 ⇒ 留空，`LoadLibraryA("AGERC.DLL")` 走 0 + 126。⛔ 没有 ALF 分支、⛔ 不读 `SYS4INI.BIN`。
2. 真实 guest 模块装载（PE 节映射 + 真实 ImageBase/HINSTANCE；资源 / 版本从镜像解析）已落地；
   `LoadIconA` 从镜像的 `RT_GROUP_ICON`（`IDI_ICON1`）拼 `HICON`，伪 HICON 只在 `.rsrc` 映射失败时作应急。
3. `kernel32_version.cpp`：`VerQueryValueA` 的字符串交**单字节 ANSI**（2026-11-05 修，见文末补记）。

## 判据（已达成）

- `--agerc` 显式给错仍拒绝启动（现状保留）；没有镜像 ⇒ `LoadLibraryA` = 0、`last_error` = 126
  （`tests/agerc_module.cpp#loadlibrary-without-image`）。
- 安装树根件装载后：`agerc_base` = 真实 ImageBase、`LoadIconA` 非空、`FileVersion` = 39
  （`tests/agerc_module.cpp` 的 `load-maps-image` / `handles-are-the-real-base` / `verqueryvaluea-string-is-ansi`）。
- 正版树（`links/raw`）**不在本单判据内**：按裁决不回退 ALF ⇒ 根件不可映射就 0 + 126。

## 不做（2026-11-05 裁决）

- ⛔ 不实现 `DATA1.ALF` 回退（原计划的 `runtime/alf_index.{h,cpp}` 与 `loadAgercModule(bytes, origin_label)`
  形状一起取消）：引擎本身也不回退，ALF 条目只留作取证对照。
- 执行 AGERC 的对话框代码（`SCOPE.md` §1 的模态 no-op stub 不变）；改 `apps/emulator-cxx`；
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
