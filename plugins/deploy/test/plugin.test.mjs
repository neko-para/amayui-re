/**
 * plugins/deploy/test/plugin.test.mjs —— **宿主侧加载**的基建契约（能加载 / 接口是闭的 / 探针能跑）
 *
 * 这一条测的是"插件**真的**能被 DSH 的 API 吃下"：`defineTool` 认不认我们的 spec、
 * `ctx.tools.register` 拿到的是不是一个 ToolDefinition、`execute` 能不能跑出结果。
 * 它依赖本包的普通依赖（`@deepseek-ai/dsh-tools` / `schemastery`）—— 那由根 `pnpm install` 装
 * （`plugins/*` 是 workspace 成员）⇒ 缺了就直接红，不跳过。
 *
 * 不测什么：不测"宿主进程是否不受 ACL 沙箱约束" —— 那要**装进 DSH 后由模型调工具**来回答，
 * 测试里跑出来的结论只代表测试进程。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const PLUGIN_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const REPO_ROOT = path.resolve(PLUGIN_DIR, '..', '..');

test('★ 插件能被宿主加载：defineTool 吃下 spec、apply 注册出 `deploy`、status 能跑', async () => {
  const mod = await import('../index.mjs');
  assert.equal(mod.name, 'amayui-deploy', 'loader 认的是这个稳定 id');
  assert.deepEqual(mod.inject, ['tools']);
  assert.equal(typeof mod.apply, 'function');
  assert.ok(mod.Config, 'Config 是部署方（人）的授权面：允许写哪些根、审计落哪');

  const registered = [];
  // ★ 审计落在**本次运行独有的临时目录**里，`finally` 整目录删掉。
  //   踩过的坑：清理原本写在第一个工具调用之后，而后面那次"被拒的调用"**也要留痕**
  //   ⇒ 文件又被造出来、再没人删，于是每跑一次测试就在 `.tmp/` 落一个 plugin-smoke-<pid>.log。
  const tmpDir = fs.mkdtempSync(path.join(REPO_ROOT, '.tmp', 'plugin-smoke-'));
  // 传**相对仓库根**的路径（插件按 repoRoot 解析落点，见 lib/audit.mjs）
  const auditFile = path.relative(REPO_ROOT, path.join(tmpDir, 'audit.log'));
  try {
    mod.apply({ tools: { register: (d) => registered.push(d) } }, { auditFile });
    assert.equal(registered.length, 1, 'apply 只注册一个工具');
    const tool = registered[0];
    assert.equal(tool.name, 'deploy');
    assert.ok(Array.isArray(tool.parameters?.properties?.op?.enum) && tool.parameters.properties.op.enum.length >= 5, 'op 必须被翻成 JSON Schema 的枚举');
    assert.equal(typeof tool.execute, 'function');

    // status 是只读的：跑一次，只要求"有结论、且结论里带证据"
    const res = await tool.execute({ op: 'status' }, {});
    assert.equal(res.op, 'status');
    assert.match(res.summary, /宿主起的子进程完整性/, '结论必须带可再校验的观察');
    assert.match(res.summary, /审计：/, '每次调用都要留痕');
    assert.ok(fs.existsSync(path.join(REPO_ROOT, auditFile)), '审计文件必须真的写出来了');

    // 校验不通过时：一个字都不做（并且如实记账）
    const bad = await tool.execute({ op: 'install-tree', out: 'C:\\Windows' }, {});
    assert.equal(bad.ok, false);
    assert.match(bad.summary, /不在允许的根内/);
    // ★ 被拒的那次**也**留了痕（append-only：拒绝同样是事实）⇒ 断言两行都在
    const lines = fs.readFileSync(path.join(REPO_ROOT, auditFile), 'utf8').trim().split('\n');
    assert.equal(lines.length, 2, '两次调用两行：status 与"被拒的 install-tree"');
    assert.match(lines[1], /ok=0/);
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
});
