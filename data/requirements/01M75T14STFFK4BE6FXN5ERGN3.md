# 默认跳过「前回正常終了しませんでした」判定（仿 --skip-logo 钩 0x4BA718；--accept-recovery-dialog 恢复）

- id: REQ-01M75T14STFFK4BE6FXN5ERGN3
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift cli boot]

## 目标

开发/CI 下每次启动都会被"上次没有正常结束"的判定拦一下（弹 `前回正常終了しませんでした．`）。
**默认跳过这个判定**（不弹），保留一个选项 `--accept-recovery-dialog` 恢复真实判定路径。做法照 `--skip-logo`。

## 取证结论（只读，已核）

- **判定读的东西**：文件 `$$AGE.TMP`（`0x40B3B9 push offset aAgeTmp`）；路径 = `<base>\<set:RegFilePath>\$$AGE.TMP`，
  `base` = `%LOCALAPPDATA%`（`set:UseAppDataFolder==1` 时，`0x409B4B push 801Ch` = CSIDL_LOCAL_APPDATA|CREATE）。
- **读的内容**：**只有"能不能打开"** —— `0x4BA713 call sub_454730` → `0x45474B push 80000000h` +
  `0x454745 push 3`(OPEN_EXISTING) + `0x454751 call CreateFileA` + `0x454759 cmp eax,-1 / setnz`。
  **函数里没有 `ReadFile`** ⇒ marker 的内容从不被读。
- **比较**：`0x4BA718 cmp eax,1` / `0x4BA71B jnz 0x4BA856` ⇒ 能打开 = 上次不正常 ⇒ 弹框
  （`0x4BA825 call j_MessageBoxA_23`，`uType=0x34` = MB_YESNO）。打不开 ⇒ 干净返回。
- **写点**：脏 = 启动时 `0x4BB037 call sub_454770`（`0x454785 push 2` CREATE_ALWAYS，**无条件、返回值不 test**，
  文件恒 0 字节）；净 = WM_CLOSE `0x4B966B call j_DeleteFileA_3`（答 YES 时 `0x4BA837` 也删）。
  ⇒ **只绕过"读"就够**，写点不用管（读者只有 `sub_4BA6B0`，其调用方只有 WinMain `0x4BAE44`）。
- **时机**：WinMain 里 `0x4BAA37`(T_Command) → **`0x4BAE44`(判定 + 弹框)** → `0x4BB037`(写脏) → `0x4BB082`(建窗口)
  ⇒ 早于 root script `0x130` / LOGO / TITLE。

## ★ 答 YES 是陷阱（用户口径需要修正）

`0x4BA82B cmp eax,6` 成立（=IDYES）⇒ 删 marker + 返回 1 ⇒ `0x4BAE4B jnz 0x4BB5AF` ⇒
**显示"启动设置"对话框然后退出**。所以"照常启动"要的是**非 YES**；"默认跳过"绝不能靠答 YES 实现。

## 已裁决（⛔ 实现者不要重新裁决；发现硬反证就停下报告）

1. **钩判定点**：新增 `kRecoveryCheckEa = 0x004BA718`，在 `machine.cpp` 里照 `--skip-logo` 那个 `if` 的形状
   （`machine.cpp:1266-1290` 旁边）把 `cpu_.eax()` 置 0（= "打不开" = 上次正常结束）。
   ⚠ **锚未用 `--trace-blocks` 观测过块起点**：实现前必须机械确认 `0x4BA718` 是块起点；
   不是 ⇒ 回退成**钩函数入口 `0x4BA6B0`，直接返回 0**（语义相同、更粗）。
2. **默认跳过**；`--accept-recovery-dialog` 关掉绕过、走引擎真实判定路径。
3. **默认跳过的语义** = "marker 文件不存在" = "上次正常结束"，**不是**"替用户答 NO"。
   机械判据：默认时 `MessageBoxA` **一次都不会被调**；传 flag 后会。
4. **不实现"真弹窗"**：全仓没有 SDL 消息框，shim 恒 IDOK、不建窗口、不阻塞 ⇒ 传 flag 只能恢复"真实判定路径"，
   弹框依旧是静默应答。⛔ 不要为此引新的 UI 依赖；若要做真弹窗，另开单。
5. **与 `REQ-01M75S724HE50D26EEMDNY3Z54`（折叠三件套）的顺序**：本单可先落。但那单把 `MessageBoxA` 改成
   "按 `uType` 给合法值"时，**MB_YESNO 的默认答案必须定 `IDNO(7)`** —— 依据就是上面 `0x4BA82B cmp eax,6`
   （答 YES 会弹启动设置并退出）。本条写进那单的实现要求。

## 判据 / 守卫

- 新增 CTest（照 `tests/skip_logo.cpp` 的形状与注册位置、同一档位）：
  默认跑 ⇒ `MessageBoxA` 的调用计数 **= 0**；传 `--accept-recovery-dialog` ⇒ **> 0**。
  ⛔ 不要靠截图/像素判，用计数这种可机械读的量。
- CLI/文档：`--accept-recovery-dialog` 的帮助文字要写明"**默认绕过**、等价于上次正常结束；传它恢复真实判定"，
  并与 `--skip-logo` 的措辞风格一致。

## 未做

- 真弹窗（无 UI 依赖）。
- `set:UseAppDataFolder` / `set:RegFilePath` 的取值来源**未取证**（用户禁止从本地样本反推）——
  本方案不依赖路径，故不影响实现；将来要断言"marker 落在哪"时再补。
