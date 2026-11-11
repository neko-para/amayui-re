# LoadData 点一下就干净退出：AGERC 缺 _SetNameLenMax@20 导出 ⇒ SAVE.BIN COMMAND=332 失败 ⇒ 错误框被 MessageBoxA 恒 IDOK 压成中止

- id: REQ-01M75SDKZGY87B8SK7GBP92PXD
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: graphs/title-loaddata.graph#click 1070 480
- severity: S1
- tags: [emulator-lift shim agerc]

## 现象

TITLE → 点 **LoadData**（客户区 `1070,480`）⇒ 进程**干净退出**（exit 0）、**无弹窗**。
用户的判据：无论如何不该走到"正常退出"。

## 根因（已实测定位，`graph` + 日志）

**emulator 的 `AGERC.DLL` 导出表不全 ⇒ SAVE.BIN 脚本执行失败 ⇒ 引擎弹错误框 ⇒ 被 MessageBoxA 的恒 IDOK 压成"中止" ⇒ 干净退出。**

完整链（一行一行都能在 `.tmp/title-loaddata/run-A.log` 里 grep 到）：

1. 点 LoadData ⇒ 引擎执行 `SAVE.BIN` 脚本，走到 `COMMAND=332`（`ADDRESS=8`）。
2. 该命令要 `GetProcAddress(AGERC, "_SetNameLenMax@20")`：
   ```
   [shim] kernel32!GetProcAddress: AGERC 没有导出 "_SetNameLenMax@20"
          （只有 _GetInstance@0 / _ShowDialog@12 / _OperateMenu@16）⇒ 返回 0 + 错误 127
   ```
3. 引擎判定失败，弹错误框（`type=0x00000112`），正文是它自己写的：
   ```
   "_SetNameLenMax@20のアドレス取得に失敗しました．ERRORCODE = 127
    デバック情報：FILE=SAVE.BIN ADDRESS=8 LINE=-1 COMMAND=-(332) DEPTH=2"
   ```
4. emulator 的 `MessageBoxA` **恒返回 IDOK(1)**（`src/shims/user32_menu.cpp:202`，且不看 `uType`）⇒
   引擎把"中止"当成用户选择 ⇒ `SendMessageA(hwnd, WM_CLOSE, 1, 0)` ⇒ main 返回 ⇒ CRT `exit(0)`
   ⇒ 日志里只剩 `kernel32!ExitProcess(00000000)` + `detail: shim 主动结束运行`。
5. 框里的正文只有 **info** 级才打（`:193`）⇒ 默认 `warn` 下**完全不可见** ⇒ 现场表现就是"点一下就正常退出了"。

## 修

1. **别再硬编码 3 个导出**：`AGERC.DLL` 是**真实模块**（已映射在 `0x30000000`，见
   `src/shims/kernel32_agerc.cpp:6` 与 `kernel32_process.cpp:954`）⇒ 从它的 **PE 导出表**枚举，
   而不是维护一张 3 行的白名单。先列出真实导出全集，与白名单做差集。
2. `_SetNameLenMax@20`（`@20` = 5 个 dword 参数、`__stdcall`）：反汇编真实实现，定参数与**返回值**语义。
3. 优先把这个导出**路由到真实代码**（模块已映射，理论上直接给 RVA 就能跑）；若它依赖别的未实现导入，
   再退回"按语义实现 Stdcall stub"。⛔ 不要先写 `return 0` 的空实现——0 可能正是引擎判失败的值。
4. **盘点同族风险**：这个 127 说明"引擎在运行期按名取 AGERC 导出"是常规做法 ⇒ 把真实导出全集里
   其余未登记的名字一起评估（哪些会被 `GetProcAddress` 到、哪些真需要实现）。

## 判据

- 跑 `graphs/title-loaddata.graph`（`--interaction-graph`）⇒ 必须**进到加载界面**：
  日志里是 `detail: interaction: task done` 而**不是** `shim 主动结束运行`；
  且**不再出现** `_SetNameLenMax@20` 的 `返回 0 + 错误 127`。
- 守卫：`GetProcAddress(AGERC, "_SetNameLenMax@20")` 必须返回真实入口（非 0），
  且它与真实 PE 导出表的差集为空（一条"白名单 ⊇ 真实导出"的机械断言）。

## 关联

- `REQ-01M75S724HE50D26EEMDNY3Z54`：把这三处"折叠"修掉（尤其 MessageBoxA 正文默认可见），
  否则下一次同类错误仍然是"点一下正常退出"。
- 重启后那条 `前回正常終了しませんでした` 弹框（`type=0x34` = MB_YESNO）也被 IDOK 答成"非 NO"，
  属于同一折叠；本单不处理，见上条。

## 复现资产

- `graphs/title-loaddata.graph`（本单新增：`await script TITLE` → `click 1070 480`）
- `.tmp/title-loaddata/run-A.log`（19 MB，`--log-level info --trace-shims`）
- `.tmp/title-loaddata/fail-A.bmp`（退出前最后一帧）
