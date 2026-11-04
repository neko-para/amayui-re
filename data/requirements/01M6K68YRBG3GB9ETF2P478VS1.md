# M1 覆盖清单归属清理（已收口：SCOPE / M2 / M4 / trap 全覆盖）

- id: REQ-01M6K68YRBG3GB9ETF2P478VS1
- type: spike
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- done_reason: 164 条静态缺口已逐组归属：永久 no-op（菜单/注册表/摇杆）、延后（音频/视频→M4；模态→M2 有限）、M2（version/shell32/shlwapi/imagehlp/imm32 + COM/VARIANT + GDI region）、hard trap（CreateProcessA）、S4 单独单；无剩余待裁。
- tags: [emulator-lift, m1, scope]

## 一句话
164 条静态缺口的归属已全部清理完：每个组都有明确落点（`SCOPE.md` §1 / M2 / M3 / M4 / hard trap / 单独单），没有剩余待裁。

## 最终归属（2026-11-04）
| 组 | 归属 | 落点 |
|---|---|---|
| 注册表 5（+未导入的 `RegCloseKey`） | **永久 no-op** | `SCOPE.md` §1 · `advapi32_registry.cpp` |
| 摇杆 3（`joy*`） | **永久 no-op**（无设备） | `SCOPE.md` §1 · `winmm_joystick.cpp` |
| 音频 11（MCI / mixer / mmio） | **延后实现（M4，可能提前）** | `SCOPE.md` §1 · `winmm_audio.cpp` · `REQ-01M6K84KHPA1XF3B1HSHXME6W0` |
| 视频 15（MF / MFPlat / EVR / DXVA2） | **延后实现（M4）** | `SCOPE.md` §1 · `mf_video.cpp` · `REQ-01M6K84KHPA1XF3B1HSHXME6W0` |
| 模态（`MessageBoxA` / AGERC 设置对话框） | **延后实现**：只做关闭窗口二次确认，不做 AGERC 复杂设置 | `SCOPE.md` §1 · M2 单 |
| `CreateProcessA` 1 | **hard trap**（自替换 / 重启路径） | `kernel32_process.cpp` |
| version 3 / shell32 1 / shlwapi 1 / imagehlp 1 / imm32 1 | **M2** | `REQ-01M6H6G6DSRG1ZRD810Z0HTH8V` |
| COM / VARIANT 5（ole32 2 + oleaut32 3） | **M2** | 同上 |
| GDI region 7 | **M2 §3.2 region** | 同上 |
| 线程 10 + SEH 2 | **硬错误（SCOPE §1）** | `SCOPE.md` §1 |
| S4 逐项复核 | **单独单** | `REQ-01M6K7PQK23EGJDJBQ6XKKE4HR` |

## 判据（收口）
- `pnpm tools lift check` 绿（310 = 146 实现 + 164 stub + 0 裸缺口；七族内 stub 0）；
- `pnpm tools requirements validate` 6/6；`ctest` 25/25；`mutate-check --tier lift` 34/34；
- 每个 stub 组的 reason 都能指到本表的一行。
