# 🐞Game Start 初始配置界面偶发纹理用错（交互正常，窗口路径，第一次打开即偶发）

- id: REQ-01M642804D7FRDHYFFRZ03C9ZH
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- repro: apps/emulator-cxx/tests/core_selftest.cpp#0x1fa 释放后重建同一编号

## 现象（用户口径 2026-10-29）

初始配置界面（Game Start → 脚本 21099）里，**整张美术图**（面板边框 / 分区标题 / 右侧按钮 / 底部三个按钮）
一个都不画，背景透出来；**交互命中正常**（几何没错，错的是"哪张纹理的内容"）。
★ 用户补充的必现路径：**先进设置界面（CONFIG1.BIN 20940）→ 返回 → 再进 Game Start**；反过来先进
Game Start 再进设置界面，则**设置界面**上会叠着初始配置界面的美术（互相污染）。"谁先谁影响对方的纹理"。

## 根因（已定位，本机窗口路径复现）

`0x1fa` 释放纹理时把记录**整个删掉**（`textures_.erase`），之后 `0x1f8` / `0x1f9` 重建同一编号时
版本号从 **1** 重新开始；而窗口路径的 GPU 纹理缓存按 `(id, revision)` 命中 ⇒ 缓存把**上一条记录**的
纹理当成当前纹理：

- 先设置界面再进配置界面 ⇒ 配置界面的美术编号被设置界面释放过，重建后 version=1 命中设置界面的旧图
  —— 尺寸/内容不符时**整张不画**（`scene_texture` 拿到的纹理仍绑定着旧内容，`build_scene_vertices`
  的 `src` 越界被裁掉）。
- 先配置界面再进设置界面 ⇒ 反过来，设置界面显示配置界面的美术碎片。

两界面用的是**同一批纹理编号**，所以"谁先谁影响对方"。

## 修法

每个纹理编号维护一条**全局单调的版本号下限**（`Machine::texture_revision_floor_`）：
`0x1f8` / `0x1f9` / 文本层画布重建都走 `next_texture_revision(id)`；`0x1fa` 删除前先
`note_texture_revision(id)` 把"用过的最高版本"记进下限 ⇒ 删掉再建也**接着涨**，缓存必然重传。

## 验证（本机，窗口路径）

- 复现脚本 `repo:apps/emulator-cxx/tests/input/title-gamestart.txt` + Option→Game Start 走一圈；
  用窗口抓图（GDI `PrintWindow`）对照：
  - 修前：面板美术整张缺失（与用户截图一致）；
  - 修后：面板边框 / 分区标题 / 按钮全部回来。
- 守卫（`apps/emulator-cxx/tests/core_selftest.cpp`，CTest `emulator-core-selftest`）：
  `0x1f8` 建 id 7 → `0x1fa` 释放 → `0x1f8` 再建，断言 `revision > 1`。
  **变异实测**：把 `0x1f8` 改回 `record.revision++` ⇒ 当场红（`revision=1`）。

## 归因（spec vs 实现）

**宿主实现错误**，不是 spec 错：`revision` 是宿主侧 GPU 缓存键，引擎不可观察，spec 里没有也不该有。
★ 但顺带查出一处**实现偏离 spec**：spec 的 `0x1fa` 说"把记录的来源文件写成无、清**纹理槽**里的对象，
**记录本身保留**（颜色键等字段不动）"；我们的模型把记录整个删了（记录与纹理槽合成一张表）。
可观察面上（`0x208` 空槽 ⇒ 0/0）等价，但正是这个删法把版本号打回 1、诱发了缓存碰撞。
⇒ 下一步：把"记录"与"纹理槽对象"拆开（或至少让 `0x1fa` 保留记录字段），登记在
`REQ-01M5BK8QMQYK7XQG707YVMQ202` 的交接项里。
