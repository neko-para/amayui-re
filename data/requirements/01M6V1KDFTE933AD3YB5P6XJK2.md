# emulator-lift · sub_46D9F0 native override（字形混合叶子，已评估可行、未实现）

- id: REQ-01M6V1KDFTE933AD3YB5P6XJK2
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, native-override, text-render, performance]

## 一句话

`sub_46D9F0`（起点 EA **`0x46D9F0`**，`.text`）**能做**函数级 native override，属**最低风险一类**：
它不是算数逻辑，而是「字形位图 → 已锁定纹理表面」的**逐像素混合叶子**。★ 评估已做完、**尚未实现**（本单 `open`）。

## 身份与契约

- 符号 `sub_46D9F0`，起点 EA **`0x46D9F0`**，`.text`；ABI **`__thiscall`**，`retn 20h` ⇒ **8 个栈参数 = 32 B**。
- `ECX = this`，类型 **`T_Font*`**：`repo:data/shape/carriers.json` 已有声明（`kind:arg` / `reg:ecx`），witness EA **`0x473EDA`**。
- 源 = `GetGlyphOutlineA` 的字形缓冲（调用方存在 `[Font+0x18ECC]`）：`a6` = 基址 · `a5` = stride ·
  `a4` 指向 4 个 dword（宽 / 高 / 原点 x / 原点 y）。
- 目标 = 纹理表面：`a2` = pitch · `a3` = bits · `a7` / `a8` = 目标 x / y · `a9` = 颜色。
- 裁剪矩形来自 `sub_46CE20`：`[mgr+0xA5D8+4*texid]+0x41C..0x428`，其中 `mgr=[Font+0x410]`、`texid=[mgr+0xB648]`。
- 位深 = `ds:dword_5518C0`（配置 `display:TextureBit`；⛔ **不是** DDraw bpp）；
  掩码格式 = `Font+0x548` / `Font+0x35584`（与除数 65 / 17、满覆盖 251 / 240 自洽）。

## 可行性判据（逐条，全过）

- 全函数 **0 条 x87**；**只有两条非栈写**：`0x46DC39`（16bpp 像素）· `0x46DE89`（32bpp 像素）；
  只读一个全局 `ds:dword_5518C0`；只调一个叶子 `sub_46CE20`；**0 个间接调用点**。
- **10 个调用点**，落在 `sub_46E3E0` / `sub_46F2D0` / `sub_471180` / `sub_4734F0` 四个函数内；
  全是**直接 `call`**，且**都不读返回值**。
- 纯（除正当的目标像素写）· 无全局状态 · 无 guest 回调 · 无 FPU · 可观察状态只有目标像素 + `EAX` / `ESP` / `EIP`。
- **Declined 条件**：目标纹理指针 = 0 ⇒ 写 `Declined`（不猜，交回解释器）。

## 收益（★ 含「不是主杠杆」的提醒）

- 在 **TITLE → OPTION** 那一次装载帧里，本函数占 **192 ms / 2615 ms** 墙钟 = **28%** 的 guest 时间；
  估算 override 后省 **150–185 ms**。
- ★ **同一帧里 guest 只占 675 ms，另有约 1.9 s 在 shim / other** ⇒ 这**不是帧时间的主杠杆**。
  本单要的是「少掉一段已证可行的 guest 时间」，不是「帧率修好」。

## 风险

1. **整数语义必须逐字复刻**：`÷65`、`÷17`、`sar edx,5` / `,3` / `,7`、`cdq;sub;and;sar` 的向零截断、
   16bpp 支再 `>>4`。
2. **虚拟时钟相位不补偿**（`repo:apps/emulator-lift/README.md` §4.3 已声明不在范围）⇒ 可能造成首帧像素差。
3. `--until ea` 指向本函数体内部会**失效**（函数体不再逐条执行，README §4.3 同条）。
4. **A4 逐字节基线不可混**：启用本项后不能拿 A4 的逐字节 trace 当基线。

## 守卫草案（三条，均未实现）

- **G1 纯算术核**（pure）：覆盖 = 0 ⇒ 一个字节都不写；覆盖值的 65 级 / 17 级 → 满值 251 / 240；满覆盖覆写；
  **目标 α = 0 ⇒ 覆写**；其余逐通道加权。变异必须红：`>>4` → `>>3` · `/17` → `/16` · `max` → `+`。
- **G2 assets 差分**：真镜像 + 真内存，跑「掩码 × 位深 × 覆盖 × α × 裁剪」矩阵，`memcmp` 目标字节，
  并比 `EAX` / `ESP` / `EIP` 与 hits / fallbacks；⛔ **不比整栈**。
- **G3 帧级 A/B**：`--native-overrides all` vs `all,-sub_46D9F0` 的**逐帧 hash 相同**且 **hits > 0**。

## 与已有单的关系（不重复开单）

- spec 侧的「三条写入支路进合同」已由 `REQ-01M61P5D8KWVYDAZNH6MVHESZF` 裁决；它登记的支路 EA
  （如 `0x46DB11` / `0x46DD5E` / `0x46DDC0` / `0x46DDD5`）可直接喂 G1。⛔ 本单不改 spec。
- native override 族挂在 M2 下：可行性试点 `REQ-01M6NZSAAD0WCNBE3RKMFF84R5`；>1s 五个热点
  `REQ-01M6PDSMKDBNW46Q8SEMAFF14S`（已收口，其范围是「boot 中 >1s」，本函数 192 ms **不在其列**）。

## 落点（实现时再动；⛔ 本单不改代码、不跑构建）

- `repo:apps/emulator-lift/src/runtime/native_overrides.cpp`（纯核 + `native*` 包装）
- `repo:apps/emulator-lift/tests/native_override.cpp`（G1 / G3 夹具）· `repo:apps/emulator-lift/tests/native_override_assets.cpp`（G2）
