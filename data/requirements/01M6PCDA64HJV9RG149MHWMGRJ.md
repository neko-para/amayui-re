# emulator-lift · boot 热点 native override：sub_436A80 LZSS

- id: REQ-01M6PCDA64HJV9RG149MHWMGRJ
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override.cpp#native-override
- done_reason: profile 选中 sub_436A80（boot 首帧 29.31% 指令权重）；按 .c/.lst 实现 LZSS（N=4096/F=18/r0=4078/length=low_nibble+3，thiscall 12B）并加入 --native-overrides。真实 boot 首帧 25.35s → 12.68s（2.00×，已有 5 项 + sub_436A80；仅 LZSS 14.32s）；首帧像素哈希 0xEBF024FEBE998B05 不变，4 帧 hash CPU 逐帧一致，25 hits/0 fallbacks；native-override 114 checks、ctest 49/49。默认仍关，显式启用。
- tags: [emulator-lift native-override boot lzss]

## 目标

boot 第一帧的 profile 选出第一热点 `sub_436A80`（S4IC/LZSS 解压），把它做成函数级 native override，
用真实 boot A/B 验证启动耗时收益，并保持像素/行为判据。

## 取证

- `sub_436A80` 的 `.lst`/`.c`：`__thiscall(this, Src, dst, info)`；`info[0]=输出大小`，
  `info[2]=压缩大小`；`info[2] != info[0]` 走 LZSS，否则 `memcpy(dst,Src,info[0])`。
- 算法逐字照 `.c`：N=4096、F=18、初始 `r=4078`、length = low_nibble+3；
  环形字典在 `this+4`（0xFEE 字节清零）。
- ABI：thiscall，`ECX=this`，栈参数 12 B（`Src,dst,info`）；native 框架按键值设置
  `EAX/EIP/ESP`，失败回退解释器。

## 实现

- `native_overrides.cpp`：`lzssDecode()` 纯解码核 + `nativeLzss436A80()`（校验 guest 范围、
  非 safe 模式用 `mem.host`、safe 模式逐段 `committed`）；解码后把环形字典写回 `this+4`。
- 已知名字表新增 `sub_436A80`（`Thiscall, 12`）；仍由 `--native-overrides` 显式启用，默认关。
- 测试：`native_override.cpp` 新增 literal / match / 非压缩拷贝三个纯用例。

## 实测（真实 boot，headless，`--max-frames 1`，无 `--real-time`）

| 配置 | 首帧耗时 | insns |
|---|---:|---:|
| 基线 | 25.35 s | 781.8 M |
| 仅已有 5 项（`_memcpy,_memcpy_0,_memset,_strlen,sub_401000`） | 18.70 s | 586.2 M |
| 仅 `sub_436A80` | 14.32 s | — |
| 已有 5 项 + `sub_436A80` | **12.68 s** | 357.1 M |

- 相对基线 **2.00×**（25.35 → 12.68 s）；首帧像素哈希/非背景数完全不变
  （`0xEBF024FEBE998B05`）。
- 4 帧 A/B：hash 与 CPU 基线逐帧一致；`sub_436A80` 25 hits / 0 fallbacks。
- `native-override` 114 checks；`ctest` 49/49。

## 下一步（不在本单）

profile 第二名 `sub_48B090`（51.6% 剩余权重）是 1/4/8bpp 调色板→16/32bpp 的转换器，
带 vtable Lock 与透明色；语义面比 LZSS 大，另立单再评估。
