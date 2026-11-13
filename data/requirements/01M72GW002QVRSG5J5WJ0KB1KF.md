# 🐞S2 emulator-lift · TITLE→Quit：最后一帧后 4.1 s 无 Present（退出时同步写存档）+ 退场 fade 层与背景同纹理（没有渐变到黑）

- id: REQ-01M72GW002QVRSG5J5WJ0KB1KF
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/src/runtime/present_sdl.cpp#postMessage
- severity: S2
- done_reason: 2026-11-13 用户确认已自然恢复（起因：修复了若干错误的 DX 实现），本单收口。(A) 4.1 s 无 Present：子单 REQ-01M7375DARF1QFZEFNWX3CAK6T（LZSS Encode native override）与 REQ-01M72QRX9X1BRE5CYNSZ18QS56（present-tail 计数）均已 done ⇒ 退出期不再是 40M 量级解释。(B) 退场 fade 层不可见：属「固定功能批次混合 α 恒 0」的症状，随 DX 实现修复而消失。⚠ 正文那处交叉引用 REQ-01M735AFW7WCG2RH89663M52D7 是笔误（该 id 不存在），正确 id 是 REQ-01M735AWXRDZKCDT4124BCFF27；后者当前仍 doing，若 (B) 确实恢复应由它自己的判据收口，不由本单代替。
- tags: [emulator-lift, quit, teardown, fade]

- tags: [emulator-lift, quit, teardown, fade]

## 一句话
TITLE 点 Quit 之后：**(A)** 窗口停在最后一帧 **约 4.1 s**（期间 0 次 Present）才退出；**(B)** 退场期**脚本每帧发两笔全屏多边形**（`Clear` 后一笔不透明黑、sprite 批之后再一笔"黑 + α 斜坡"= 渐变覆盖层），但**这两笔在我们这里一个像素都没画上去**：`[draw] fa=` 实测**真正参与混合的 α = 0**。两者**不是同一个原因**：(A) = guest 侧同步拆除（写存档 + 释放资源）× 解释器速度；(B) = 覆盖层用的是 `MODULATE(TEXTURE, TFACTOR)`，α 取自**纹理**（当前绑的那张 α=0）而**不是顶点 α** ⇒ 整笔全透明。

## 复现（2026-11-10，win32 x64 / RelWithDebInfo，`apps/emulator-lift`）
```bash
LIFT_SLOW_DRAW=1 LIFT_SLOW_DRAW_MS=0 LIFT_DRAW_CALLSITE=1 LIFT_DRAW_LOCALS=004B4335 \
  ./out/emulator-lift.exe --game-dir ../../links/install --appdata <私有 tmp> --skip-logo \
  --present headless --raster cpu --audio off --native-overrides all --max-frames 136 \
  --input-script <脚本>        # 脚本：at frame 130 / click 1190 630
```
（`--raster cpu` 才能看到 `fa=`；gpu 光栅不跑 CPU 那条聚合。墙钟读数按旧口径用 `--present window`。）

## (A) 4.1 s 卡住 = guest 同步拆除（宿主不是原因）
- 判据 1（无 Present 的尾部）：`LIFT_FRAME_TRACE=1` 的 `Σ[frame] wall = 26.846 s` vs `real = 30.971 s` ⇒ **最后一帧之后 4.125 s 内 0 次 Present**；同期 `user 35.230 s > real 30.971 s`（CPU 满载）、`--profile` 的 `host` 桶仅 `160.7 ms`，`guest+interp` 占 87%。
- 判据 2（排除宿主）：`--audio off` 4.13 s vs `--audio sdl` 4.18 s（差 ~50 ms）。
- 判据 3（他做什么）：`sub_40AAE0`（写 SAVE.DAT）→ `sub_437480` → `sub_4367B0`（LZSS + `sub_436550` self 1.21 s / 150,086 次），点击后窗口 inclusive **3.16 s**；落地件只在**最后一次 Present 之后**出现。
- ⇒ 真机同一段只要 ~100 ms；我们慢的是**解释执行 LZSS + 文件 I/O**。

## (B) 渐变**存在**，是两笔脚本多边形；我们把它画成了全透明
- **来源**：脚本 **opcode 0x15（画多边形）**，handler `0x432168`（`mov dword ptr [esi+ecx*8+5D8F4h], 15h`；顶点数 `0x432177 sub_41BF50(9)`，其余参数 `sub_42AEA0(5/6)`/`sub_42BF60(7/8)` 从 VM 栈取）。顶点色在这一层解码（`ROR(key ^ ROL(raw,11),25)`，key=`[engine+0x5EC8C]`），记录进 `engine+0x428` 的 display-list（payload 15 dword），每帧由 `sub_4AF1C0`（RenderPolygon）→ `sub_49E390` → `sub_49E310` 画出。**α 是脚本参数，逐帧重发**。
- **帧结构**（`[draw] frame=` 分组，f133/f134/f135 实测）：`Clear(0,0,3,0x00000000,1.0,0)` = **每帧清成黑** → 多边形#1（黑，顶点 α=255，**在 sprite 批之前**，压黑无害）→ 60 笔 sprite（真正的 TITLE 画面）→ 多边形#2（黑，顶点 α = 0,13,27,…,255，**在 sprite 批之后** = 渐变覆盖层）→ EndScene/Present。
- **两笔都没有纹理**：id 在 payload `v40[7]/v40[8]`，读到的就是 `-1/-1`（`sub_49E390` 的 `edi` 因此 = 0）。
- **为什么一个像素都没画上**（新读数，`[draw] fa=` 是本批真正参与混合的 α 的 n/均值/最小/最大）：两笔都是 `fa=894575/0/0/0` 与 `919601/0/0/0` ⇒ **α 恒 0、整笔全透明**。stage0 两段都是 `MODULATE(TEXTURE, TFACTOR)`：α 取**纹理**（当前绑的是很久以前留下的那张，`texserial=22`，α=0），**顶点 α 不参与**；`ALPHABLENDENABLE=1`、`blend=5/6`。
- **引擎自己的多边形状态**：`sub_49E390` 只在 `edi==1/2`（绑到 1~2 张纹理）时把 `COLORARG2/ALPHAARG2` 设成 **DIFFUSE**（`0x49E608–0x49E660`）；`edi==0` 时**一条不设**，依赖调用方留下的状态。而调用方那条 `sub_4C19E0` 是 `COLORARG2=DIFFUSE(0x4C1A29)` → `ALPHAARG2=TFACTOR(0x4C1A57)` → **`COLORARG2=TFACTOR(0x4C1A9C)` 覆盖**（中间无 Draw）⇒ 到这两笔时 arg2=TFACTOR。
- **没有别的路能变黑**（独立复核，frame 133–150 窗口）：`Clear`=1/帧、`DrawPrimitive`=2/帧、`DrawIndexedPrimitive`=60/帧；`LockRect/UnlockRect/StretchRect/UpdateSurface/ColorFill/SetRenderTarget/GetRenderTargetData/UpdateTexture` **全 0** ⇒ 没有 FillTexture、没有 RTT、没有粘贴。也**没有 caps/config 分叉**（`sub_4AF1C0`/`sub_49E390`/`sub_41BF50`/`sub_4A2280` 只有"创建/加锁失败"错误臂）。
- ⇒ 渐变**只能**是这两笔多边形；它们在我们这里 α=0 全透明，所以"看不见"。

### 2026-11-11 更新（本轮实测，根因上移一层）
- 2D 图层那 60 笔 `DrawIndexedPrimitive` 原先被 `drawPrecheck` 以"只支持 SetFVF"**整批拒绝**（60 笔/帧全 `D3DERR_INVALIDCALL`）。本轮实现了**顶点声明路**（POSITION FLOAT3 在 stream0 + TEXCOORD0 FLOAT2 在 stream1 去交错成交错缓冲，`declLayoutOf`/`buildDeclVertices`），60 笔现在成功并写满全屏。
- 但**所有固定功能批次的混合 α 仍是 0**（`[draw] fa=<n>:0/0/0`，含顶点色为白的图层批；`[draw] texa=` 恒 `0..0`）⇒ 屏幕上可见的 TITLE 画面**全靠宿主 `ID3DXSprite` 合成**。⇒ (B) 的根因**不是**这两笔多边形的 stage arg，而是**纹理 α=0**这一条系统性问题：见 `REQ-01M735AFW7WCG2RH89663M52D7`（固定功能批次 α 恒 0）。修它之前，(B) 不可能靠"改 arg2/TFACTOR"修好。
- 另：`--raster cpu` 下 60 笔图层批 + 2 笔多边形都 `fa=0`，所以"新增的图层批画上去但画面没变"是**预期**，不是新的渲染 bug。
## <待核>（决定 (B) 怎么修；现在只剩一个分叉）
真机上这一笔**为什么α 不是 0**（三种可能，互斥，且都能用代码/读数判）：
1. **顶点色本该参与**：`sub_49E390` 的 `edi==1` 分支（带纹理）会把 arg2 设成 DIFFUSE ⇒ 若脚本这一笔在真机上传的纹理 id ≥ 0（不是 `-1`），α 就直接来自顶点 α（与"黑 + 0→255"完全吻合）。**要查**：脚本层给 opcode 0x15 的纹理参数为什么是 `-1`（同一份脚本在真机上也是 `-1`？还是我们这边解析/取值不同）。
2. **stage0 的那张"残留纹理"在真机上不是这张**：我们的 `SetTexture` 绑定是**几万条指令之前留下的**（最后 300 条 shim 里一次 `SetTexture` 都没有），而真机 D3DXSprite 的 `Begin/End` 会**保存并还原**设备状态 —— 我们的 `ID3DXSprite` 是宿主实现，从不碰也从不还原 stage0/纹理绑定。**这是最可疑的"我们实现错了"的地方**。
3. 真机 stage0 arg2 是 DIFFUSE（引擎写序不同）—— 与"`sub_4C19E0` 中间无 Draw"矛盾，除非真机在 sprite 批之后又设了一次。

## 判据（修好后必须成立）
1. (A) run summary 能给出**确定性的** `present-tail`，且退出期不再是"40 M 量级的 LZSS 解释"——或明确登记为**已知性能偏差**（真机 ~100 ms）。
   - **计数器部分已交付**（2026-11-10）：子单 `REQ-01M72QRX9X1BRE5CYNSZ18QS56`，`verify: apps/emulator-lift/tests/input_injection.cpp#present-tail-stamp`。
   - 剩下的是"把它降下来"：spike `REQ-01M72QS2WFNP61Y6EGFAEVRFG4`（整函数 override `sub_4367B0`）**本轮未实现**。
2. (B) 退出序列的**连续帧像素均值单调下降**且**至少一帧** ≈ `0x00000000`（ESC 支可作参照）。★ 判据必须落在**像素**上：这 18 帧"被提交过"不算通过（本轮就是"提交了、`fa=0`、一个像素没变"）。

## 未做
- 未定位上面 `<待核>` 的那一个分叉 ⇒ 未改 (B) 代码，未 commit。
- 未实现任何 override；(A) 的性能部分仍 open。
- 已交付诊断（可重跑，非结论）：`[draw]` 扩了 `frame= / texserial= / tfactor= / alpha= / d0= / uv= / fa=`；`LIFT_DRAW_CALLSITE=1`（`[draw-call]`/`[sts]`/`[vblock]` 调用链）与 `LIFT_DRAW_LOCALS=<EA>`（读绘制调用点的局部）；口径见 README §4.3.1。
