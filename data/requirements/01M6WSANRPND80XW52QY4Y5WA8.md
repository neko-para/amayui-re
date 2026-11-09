# 两次运行 / 两张 BMP 的像素比对没有落点：做成 tools/lift.mjs diff 子命令（两张 bmp → max-diff / 差异像素数 / 退出码）

- id: REQ-01M6WSANRPND80XW52QY4Y5WA8
- type: req
- status: done
- parent: REQ-01M6XVZE3AN0FKFZA52VV23WD9
- verify: tools/test/lift-diff.test.mjs#diff-changed
- tags: [emulator-lift tooling]

## 缺口

手工 A/B（“同一份输入脚本，改一处实现，两张图差在哪”）现在只能手写 PowerShell 读 BMP。没有工具 ⇒ 每次重写、没有退出码语义、结论只活在会话里，⛔ 不能当无人值守判据。

## 要比的是哪两份产物

- `--dump-frame`：guest 后缓冲（BMP）。
- `--present-screenshot`：**窗口回读**（BMP；`--present-screenshot-frame N` 定帧）。
两者是**两份独立产物**（`repo:apps/emulator-lift/README.md` §4.2b）：1:1 时可逐像素对照；缩放 / letterbox 下只能比内容矩形。⇒ 比对工具必须**先报尺寸与像素格式**，尺寸不同直接判红，⛔ 不许静默裁剪。

## 最小接口（父级口径）

```
pnpm tools lift diff <a.bmp> <b.bmp> [--allow-max-diff N]
```

- 输入：两张 BMP（允许 24/32bpp、top-down/bottom-up —— 先归一化再比）。
- 输出（stdout，人类可读 + 一条机器可读行）：`size`、`max-diff`（任一通道最大差，0..255）、`diff-pixels`（差 > 0 的像素数）与各自占比。
- 退出码：`0` = 完全相同（max-diff 0）；`1` = 有差异；`2` = 输入不可读 / 尺寸或位深不一致。
- `--allow-max-diff N`：容许量化损失（先例：`repo:tools/ui-bake.mjs` 的逐像素判据 —— 边缘反锯齿差 1 是有意的，不是回归）。

## 父级裁决（2026-11-08）：值得做成基建，但**不在本轮**

本轮只登记。候选落点：`repo:tools/lift.mjs` 新子命令（基建、可当门禁）优于 `.tmp/` 临时脚本（`.tmp/` 不入 git、下次会话就没了）。

## 判据（做的时候）

- 同图自比 ⇒ 0 差异、退出码 0；已知差 1 像素的夹具 ⇒ 报得出那个像素、退出码 1。
- 尺寸不同 ⇒ 退出码 2 且**不**输出 max-diff（避免“比了个错的东西”）。
- 有 pure 守卫（`repo:tools/test`），且不依赖游戏安装（用生成的 BMP 夹具）。
