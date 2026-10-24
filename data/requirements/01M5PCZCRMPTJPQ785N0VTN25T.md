# 残差第二十一批：「区间之下」的目的 · 文本层画布带

- id: REQ-01M5PCZCRMPTJPQ785N0VTN25T
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-draw-plan.test.mjs#★ 位置档位：纹理编号 20..29、位置 1.4、场景缩放 2 ⇒ x = 3（乘缩放后**半上取整**）
- tags: [spec residual]

# 残差第二十一批：「区间之下」的目的 · 文本层画布带

## 一句话

「区间之下」是给低档不透明离屏画布**垫底**；纹理编号 `[20,30)` 定名为**文本层画布带**。两条残差关闭，各劈出一条后续项。

## 区间之下

- 命令档用画布前先清空；低档画布**不透明**、高档带透明度；画布整幅使用（种类 1 表面拷贝，种类 0/2 双画布合成）。
- 低档未画区域是不透明底色会盖住底下内容 ⇒ 先把本段起点之前的条目垫进画布；高档未画区透明 ⇒ 不需要。
- 同帧两次发射是后果；不置已消费是为保住收尾扫描那一次。门是能力档代理，不是画布格式。
- 关闭 `RES-SCENE-COMMAND-BELOW-RANGE-ROLE`；新 `RES-SCENE-COMMAND-BELOW-RANGE-OMITTED-KIND3`。

## 文本层画布带

- 文本子系统按层号 i 自建 `20+i` 离屏画布（10 层 ⇒ 20..29）；CPU 写入后同号入队绘制项。
- 画布按适配比预缩放；消费端位置乘同一比 + 半上取整，纹理目标用逆缩放矩阵抵回。
- 旧负结论错在只扫 `push 14h..1Dh`；常量自建编号是 31/32/36/37/38。
- 关闭 `RES-TEX-DRAW-CONSUMER-BELOW-RANGE`；新 `RES-TEX-LAYER-CANVAS-COLLIDE`。

## 实现

逻辑不变；`SCREEN_BAND_*` 注释改为“文本层画布带”。

## 仍开着

种类 3 未覆盖区域是否不透明底色 · 同号撞车覆盖 · 适配比方向（既有）· 真实绘制 S2+ · 保存读侧。

## 判据

`pnpm test` 1020/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 336/336 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-SCENE-DRAW-ORDER` / `CORE-OPS-TEXTURE-DRAW-CONSUMER` · `docs/02-engine/spec-audit-2026-10.md` §38。
