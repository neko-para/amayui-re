# `tools/trusted.json` —— 被信任子系统（CTest 不跑）

> 结构化数据是不透明数据；字段 / 不变量 / 操作的**真源**是
> `node tools/trusted.mjs --describe`（本文件只讲怎么查、怎么改）。

## 怎么查

```bash
pnpm tools trusted status     # 每组：at / glob 命中文件数 / frozen|changed
pnpm tools trusted check      # 每个 glob 至少命中一个 tracked 文件（红 ⇒ 退出码 1）
```

`frozen` 的判据只有一条：`git diff --quiet <at> -- <globs>` 退出 0。
reconfigure 时 `apps/emulator-lift/CMakeLists.txt` **段尾**读本文件，把该组 CTest 逐条标
`DISABLED TRUE`（`ctest -N` 计数不变，跑起来是 `***Not Run***`）；`-D LIFT_ALL_TESTS=ON` 强制全跑，
`ctest -R <名字>` 单条强制跑。git 不可用 / JSON 坏了 ⇒ 全跑（安全方向）。

## 怎么改

```bash
pnpm tools trusted mark <组> --write    # 把该组的 at 写成当前 HEAD（缺省 dry-run）
```

* 只放**真的已冻结**的组：组内任何文件一改，`status` 就 `changed`，该组 CTest 立刻恢复运行。
* 组 → CTest 名字的映射**只在 CMake 段尾**（`legacy-native` / `legacy-ir-golden`）；⛔ 不新增 `add_test`、
  不改注册位置（判据是注册位置，见 `AGENTS.md` §12.4）。
* ⛔ 字段语义不要抄进别处：以 `node tools/trusted.mjs --describe` 为准（`describe` 输出字段表、不变量与操作）。
