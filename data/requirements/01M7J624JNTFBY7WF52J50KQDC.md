# 控制层 · 探索手感缺口（7 条：采样≠门 / 无 --until-pos / click 无回执 / headless 截图全黑 / 缺省值 / 不 fail-fast / 缺录制）

- id: REQ-01M7J624JNTFBY7WF52J50KQDC
- type: req
- status: open
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- tags: [emulator-lift, control-plane, ux]

## 一句话

M0/M1 落地后，**探索者实跑一轮**留下的"手感缺口"清单（7 条）。每条只记录现象与代价，⛔ 不承诺实现顺序。

## 现象与代价（都来自 2026-11-16 的 GAMESTART / SN0000 探索实跑）

1. ★ **`sample` 与 `await position` 的评估点不同，没写进 `--describe`**：`sample` 停在**块边界**（`paused_at:block`），
   位置门在**读点**判定 ⇒ 两者命中率可以完全不同（实测：GAMESTART 的门 7/7 命中，而 `sample` 采到该区间 **0/47**）。
   代价：差点用一个不成立的推理否掉一个 7/7 能命中的门。**至少**要在 `--describe` / `control-plane.md` 写一句
   "采样命中率不能推门命中率，反之亦然"。
2. **没有"按 pos 条件暂停"**（只有 `--for-ms` / `--for frames`）：想停在某个区间只能猜 `--for-ms`，
   且停点在 `block` / `present` 之间跳 ⇒ 读数只能当**分布**，不能复现到"同一点"。
3. **`click` 没有施加回执**（`applied_at {pos, state_seq}`）：点"丢在某个瞬间"时，分不清"点被丢掉"与"点被当成跳过对话"。
4. **`screenshot` 在 `--present headless` 下是全黑**（1280×720、`source=guest-backbuffer`）⇒ "点对没点对"不能靠截图，
   只能靠 `script`/`pos`。⚠ 与判据 5 不矛盾（5 只断言"同一帧 + hash_still_matches"），但**别把截图当画面证据**。
5. **缺省值对交互式探索不友好**：`--control-orphan-timeout` 缺省 10 s（命令之间思考久了端点自己收口，实测得显式给 600）；
   `emu-ctl list` 的缺省 root 是 `.tmp/emu-ctl` 而不是上一次的 `--control-root`（第一次打空）。
6. **配方失败不 fail-fast**：一次失败的 gate run 要付满 `--time-limit`（实测一次 60 s）。`--input-plan` 没有"失败即退"。
   ⚠ 这条与"判据要能红"无关，纯粹是探索成本。
7. **缺"探索 → 配方"的录制**：这轮 plan 是手抄进 `/tmp` 的 ⇒ M2（录制导出）确实是真需求，不是锦上添花。

## 判据（真要做时，每条能红）

- 1 ⇒ `--describe` 与 `docs/03-emulator/control-plane.md` 里都有那句话（守卫：`tools/test/emu-ctl.test.mjs` 断 `--describe` 含该段文字）。
- 2 ⇒ 新增 `resume --until-pos <SCRIPT> <lo> <hi>`：在区间内停 ⇒ `paused_at` 落在该区间；等不到 ⇒ `timeout` + 谓词已清。
- 3 ⇒ `click` 的回执里带 `applied_at`；同一暂停点两拍 `click` 后 `applied_at.pos` 单调前进。
- 5 ⇒ `emu-ctl list` 缺省能读到"最近一次用过的 root"（写一个 `~/.cache` 之外的仓内小文件？**先裁决再动**）。
- 6 ⇒ `--input-plan-fail-fast`：门失败立刻退（exit 5），不等 `--time-limit`。

## 未做 / 边界

2 / 3 / 6 是**新能力**，⛔ 不要顺手塞进 M0/M1 的单里（那两张单已收口）；要做就按上面判据另开子单。
4 是**已知事实**，不改：headless 没有真实窗口，后缓冲的内容由 `--present` 档决定。
