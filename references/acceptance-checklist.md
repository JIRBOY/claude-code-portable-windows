# 验收清单

按"项 / 基准 / 实际 / 结论"记录。任何一项没跑，都写"未验证"，不要用"整体符合"代替。

## A. 构建完整性

| 项 | 基准 | 方法 |
|---|---|---|
| 原始 claude.exe 保留 | 源文件与快照 SHA-256 一致 | `Get-FileHash` 两处比对 |
| 启动器产物 | x64、静态、无 VC++ 运行库依赖 | 编译报告 + 导入表检查 |
| 运行时文件数 | Node/Git 等目录源目标文件数一致 | 递归计数 |
| Python 依赖 | `uv pip check`、`pip check` 通过 | venv 内执行 |
| 代表性 import | 必需库全部可导入 | 逐个 `import`，记录失败项 |
| Chromium（可选） | 能启动并正常关闭 | Playwright 启动脚本 |

## B. 启动器行为（替身测试）

用 `mock_claude.c` 编译的假 `claude.exe`，避免真实计费。

| 项 | 基准 |
|---|---|
| 普通参数 | 原样传递 |
| 空格参数 | 不被拆分 |
| 内嵌双引号 | 按 Windows 规则还原 |
| 尾反斜杠 | 不吞引号 |
| UNC 路径 | 可解析 |
| `--` 之后 | 视为字面参数 |
| stdin/stdout/stderr | 原样连通 |
| cwd | 子进程 cwd 正确 |
| 退出码 | 非零退出码原样返回 |
| 宿主变量 | `BASH_ENV`、`GIT_CONFIG_*`、代理、`ANTHROPIC_*` 不继承 |
| 必需变量缺失 | 明确失败，不静默回退 |
| 权限参数覆盖 | 拒绝并报错 |
| 坏配置 | 非法 UTF-8、NUL、前向引用、空值均失败，且不回显敏感值 |

目标：所有替身用例通过，记录通过数与总数。

## C. 环境隔离

| 项 | 基准 |
|---|---|
| PATH | 只解析到便携 node/python/bash/pwsh |
| 宿主 Node/Git/Python | 从 PATH 移除后仍可运行 |
| 宿主用户 site-packages | 不被加载 |
| Git 全局配置 | 指向包内 |
| 临时目录 | 位于 `data\tmp` |
| 缓存 | 位于 `data\cache` |
| npm/pip/uv 配置 | 不读宿主配置 |

## D. 移动测试

1. 把整个目录复制到新路径，路径同时包含空格和中文。
2. 临时隐藏原 Python 基座目录（模拟旧路径失效）。
3. 在新副本中运行：

```text
node --version
bash -lc "uname -s"
runtime\venv\Scripts\python.exe -c "import sys; print(sys.base_prefix)"
runtime\venv\Scripts\pip.exe check
runtime\venv\Scripts\pytest.exe --version
```

基准：全部成功；`sys.base_prefix` 指向副本内的 `runtime\uv-python\<版本>`。
4. 恢复原基座，确认没有副作用。
5. 用不含真实凭据的副本测试，避免复制凭据。

## E. 真实 Claude 验收

| 项 | 基准 |
|---|---|
| 登录/鉴权 | 能完成一次真实调用 |
| 全局规则 | 自定义 `CLAUDE.md` 生效 |
| 技能 | `init` 事件中技能数量与预期一致 |
| 工具 | Bash 与 PowerShell 各产生一次真实 `tool_use` / `tool_result` |
| 权限模式 | `init` 中 `permissionMode` 符合设计 |
| cwd | 与启动目录一致 |
| 无副作用残留 | 测试产生的 session/machineID 状态按需清理 |

建议用随机 nonce 文件验证工具副作用，而不是只看日志。

## F. 凭据与隐私

| 项 | 基准 |
|---|---|
| 凭据位置 | 只在 `data\.claude`，不在 `src`/`tools`/报告/快照 |
| 扫描 | `sk-ant-`、`ghp_`、`AKIA`、私钥头、绝对路径、用户名 均无命中 |
| 报告内容 | 只有路径、数量、哈希；无令牌值 |
| 包外快照 | 路径记录在案，但内容不随包分发 |
| 分享边界 | 明确列出不可分发项 |

## G. 干净机器边界

- 若没有第二台物理机器，只能写"已在屏蔽宿主 PATH、隐藏原基座、隔离副本中验证；未实机换机验证"。
- 外部服务（API、搜索、邮件、数据库）依赖网络与有效凭据，不计入自包含范围。

## 结论模板

```text
状态：本机自动化验收通过 / 部分通过 / 失败
通过：A 6/6，B 51/51，C 7/7，D 33/33，E 12/12，F 5/5
未验证：第二台物理 Windows 机器
已知失败：<逐条列出，不省略>
回退：<快照路径与恢复步骤>
```
