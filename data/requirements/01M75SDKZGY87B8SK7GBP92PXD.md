# LoadData 点一下就干净退出：AGERC 缺 _SetNameLenMax@20 导出 ⇒ SAVE.BIN COMMAND=332 失败 ⇒ 错误框被 MessageBoxA 恒 IDOK 压成中止

- id: REQ-01M75SDKZGY87B8SK7GBP92PXD
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/loader.cpp#agerc-exports
- repro: graphs/title-loaddata.graph#click 1070 480
- severity: S1
- tags: [emulator-lift shim agerc]

## 结果

**真实导出 21 条 / 旧白名单 3 条 ⇒ 差集 18**（`links/install/AGERC.DLL`，PE32 ImageBase `0x10000000`，export dir RVA `0xA8790`；
18 个名字的清单见 `docs/03-emulator/platform-effort.md` §3.9，与那里记的 18+3 一致）。

## `_SetNameLenMax@20` 的真实语义（已反汇编）

`__stdcall`，5 dword，`ret 0x14`。RVA `0x32EF1` = `jmp 0x100367D0`（rel32 thunk，换基址后仍正确）；体在 `0x100367D0`：
`mov eax,[ebp+0Ch]` / `mov ecx,[eax]` / `mov [0x100A9000],ecx` / `xor eax,eax` / `retn 14h`
⇒ **把 `*(int*)arg1` 存进 AGERC 的 `.data` 全局 `g_nameLenMax`（VA `0x100A9000`，初值 `0x12`）**，**返回值恒 0**，
其余 3 参不读，无导入、无调用。该全局只被 AGERC 自己的换行循环读，而那条路是 `_ShowDialog@12`（SCOPE §1 stub）⇒ 引擎观测不到。

★ 原单"0 可能正是失败值"的担心**不成立**：0 就是真实返回值；失败是"**地址**取不到（127）"。

## 为什么是 stub 而不是"路由到真实代码"（硬反证）

AGERC 的 ImageBase `0x10000000` 被 **guest 堆**占 ⇒ 总被换基址到 `0x30000000`；而 `image.cpp:4` 明写"固定 imageBase ⇒
**不做重定位**"⇒ `.reloc` 从不应用；该函数唯一的绝对引用（`0x100367F9`）**正是 `.reloc` 里的 HIGHLOW**（RVA `0x367FB`）
⇒ 在 `0x30000000` 跑它会把值写进 **`0x100A9000` = 堆区间**（**静默写坏 guest 数据**）。
⇒ "给 RVA 就能跑"被证伪，必须 stub（读 `*(int*)arg1` 前先问 committed；记进宿主侧模型；返回 0）。

## 机制（替代白名单）

`GetProcAddress(AGERC, name)` 三档：①**已登记实现**（4 个 stub）→ thunk；②**真实 PE 导出表里有但未实现** →
`addRuntime` 的 **unimplemented thunk（非 0，调用即响亮失败**并打印名字+reason）；③**PE 里也没有** → 0 + 127（与真机同形）。
导出表从**已映射镜像的字节**读（节表 RVA→offset）；读不出会 warn，⛔ 不当成"没有导出"。

## 守卫与变异

- `agerc-exports`（`tests/loader.cpp`）：先断言 `count > 0` 防真空绿；**每条真实导出都解析成非 0**；
  `_SetNameLenMax@20` 必须解析到**已登记实现**（`fn != nullptr`）且**真返回值 0**。
- 变异：注册名改成 `_SetNameLenMax@16` ⇒ **红**（`def->fn != nullptr` + `expected 0 got 0x7FFE0678`）。
  ★ 只删注册时"非 0"**仍绿**（落到 unimplemented thunk）⇒ 这就是必须留第二条断言的原因。

## ★ 新证据（改变共识，已写进 `platform-effort.md` §3.9）

引擎按名取 AGERC 导出的**名字来自剧本数据**（`LoadLibraryA`@`0x42FF36` + `GetProcAddress`@`0x42FFB1` / `0x422AF0`，
失败串 `%sのアドレス取得に失敗しました` @`0x42FFD2`）⇒ **任何一条导出都可能被运行期点名**，"静态 0 引用" ≠ "不会取"。
这是"三档机制"（尤其第 2 档要**响亮失败**）的存在理由。

## e2e 复核（父级，合并构建 `out-merged`）

`graphs/title-loaddata.graph` ⇒ **exit 0**，日志：
`AGERC!_SetNameLenMax@20 ⇒ 已登记实现的 stub thunk` · `agerc!_SetNameLenMax@20: … *arg1 = 18 ⇒ 返回 0` ·
`detail: interaction: task done`（不再是 `shim 主动结束运行`）。
**截图取证** `.tmp/title-loaddata/loaddata-last.png`：**LOAD 界面正常打开**（`LOAD 000..009` / `保存情報なし` / 页码 / `戻る`）。
⚠ 同跑仍会弹 `前回正常終了しませんでした`（另一张单 `REQ-01M75T14STFFK4BE6FXN5ERGN3`，与本单无关）。

## 未做

- 其余 17 条**不实现**：解析成 unimplemented thunk（保持非 0）。真跑它们需要**应用 `.reloc`**（另一条单，`image.cpp`）。
- 两处过期注释不在边界内：`kernel32_process.cpp` 日志"三个导出仍走 stub thunk"、`register.cpp/h` 的"三个导出"字样。
