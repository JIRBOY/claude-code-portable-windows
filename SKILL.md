---
name: claude-code-portable-windows
description: >
  Build a self-contained, relocatable Claude Code installation on Windows 10/11 x64.
  Use when the user asks for a portable/U-disk/offline/no-admin Claude Code, a custom
  launcher with isolated environment, or wants to wrap Claude Code in a local GUI.
  Covers runtime assembly, a native environment-isolating launcher, config migration,
  relocation testing, and an optional local web GUI. Never bundles credentials or the
  proprietary claude.exe. 触发词：便携版 Claude Code、Windows 便携、绿色版、U 盘运行、
  自包含、离线安装、portable Claude Code、relocatable、scc.exe、本地 GUI。
license: MIT
---

# Claude Code Windows 便携版构建

把一个官方 Claude Code 安装变成可整体复制、移动盘符、不污染宿主环境的目录。产物的核心是原生启动器 `scc.exe`，不是批处理包装。

## 适用与不适用

适用：
- 目标机没有管理员权限，或不允许安装 Node/Git/Python。
- 需要放在 U 盘、移动硬盘、内网隔离机器上使用。
- 多个项目共用一个 Claude 配置，但不想污染宿主 `%APPDATA%`、PATH、Git 配置。
- 想在便携包上再套一个本地 GUI。

不适用：
- 需要多人共用、集中管理、审计的部署（应使用官方企业方案）。
- 需要分发 `claude.exe`、账号凭据或第三方付费技能的场景。
- Linux/macOS（本技能只覆盖 Windows）。

## 三条硬约束（先确认，再动手）

1. **不分发 `claude.exe`。** 它是 Anthropic 的专有二进制。便携包可以自用或内部制作，但公开仓库只能提供构建脚本，让使用者从自己的官方安装中获取。
2. **不分发凭据。** `data\.claude` 里的登录态、`ANTHROPIC_AUTH_TOKEN`、第三方中转地址、sessions 都不能进入源码、日志、报告、仓库。迁移前先定好哪些文件排除。
3. **技能与插件逐个确认授权。** 第三方付费/私有技能、个人科研技能不能因为"打包方便"就随包公开。

## 阶段 0：确认验收与回退

开工前写清四件事，缺一不可：

| 项 | 示例 |
|---|---|
| 验收 | 在屏蔽宿主 PATH 的进程中，`scc.exe` 能调用随包 node/bash/pwsh/python，且移动目录后仍可运行 |
| 证据 | 命令输出、退出码、SHA-256、JSON 报告 |
| 回退 | 原 `claude.exe` 快照、目录整体备份、凭据单独可恢复副本 |
| 边界 | 未在第二台物理机器实测就明确标注"未实机验证" |

## 阶段 1：目录布局

```
<PORTABLE_ROOT>\
  claude.exe                 # 使用者的官方安装，不提交版本库
  scc.exe                    # 原生启动器
  src\                       # 启动器源码
  config\portable.env        # 全部环境变量
  runtime\
    node\  git\  pwsh\  uv\
    uv-python\<版本>\        # 独立 CPython
    venv\                    # 可重定位虚拟环境
    ms-playwright\           # 可选
    bin\                     # 跨路径工具 shim
  data\
    .claude\                 # 配置、技能、凭据（敏感）
    home\ AppData\ tmp\ cache\
  tools\                     # 构建与验收脚本
  .snapshots\                # 回退快照
```

原则：`runtime` 是可重建产物，`data` 是用户态数据，二者不要混放。所有路径从启动器自身位置推导，配置文件里不出现盘符和用户名。

## 阶段 2：准备运行时

1. **Node**：官方 Windows x64 压缩包，解压到 `runtime\node`。不要用宿主 Node。
2. **Git for Windows**：完整安装版解压，保留 `bin\bash.exe` 与 `usr\bin`（Claude Code 的 Bash 工具依赖）。
3. **PowerShell 7**：官方 zip 包，放到 `runtime\pwsh`。
4. **uv + Python**：用 uv 安装独立 CPython 到 `runtime\uv-python\<版本>`，再基于它创建标准 venv 到 `runtime\venv`。把版本名写入 `runtime\uv-python\ACTIVE_PYTHON`，由启动器在每次启动时修正 `pyvenv.cfg` 的绝对基座路径。
5. **依赖**：在 venv 内安装常用库，生成 `requirements-lock.txt`。`uv pip check` 与 `pip check` 必须通过。
6. **Playwright（可选）**：`PLAYWRIGHT_BROWSERS_PATH` 指向包内目录，安装 Chromium。

每个下载都要固定版本并校验 SHA-256。记录来源、版本、校验值和验证命令。

## 阶段 3：构建启动器

源码与编译脚本在 `assets/src` 和 `scripts`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\get-toolchain.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-launcher.ps1 -Root <PORTABLE_ROOT>
```

启动器必须做到（实现细节见 `references/launcher-design.md`）：

- 从自身所在目录推导根路径，支持空格、中文、UNC、尾反斜杠。
- 只继承 Windows 基础变量，再叠加 `config\portable.env`；不继承宿主 PATH、Python、Git、npm、代理、API 变量。
- `${NAME}` 只允许引用基础变量或本文件前面已定义的变量；未定义、空值、前向引用、NUL、超长一律失败退出。
- 启动前校验 node/pwsh/bash/uv/venv python 和 `claude.exe` 都存在。
- 每次启动重写 `runtime\venv\pyvenv.cfg`，使 venv 跟随目录移动。
- 根据策略强制权限模式，并拒绝用户用参数覆盖（自用可 `bypassPermissions`；对外分发不要这样做）。
- 原样传递退出码、stdin/stdout/stderr、cwd。

编译用静态 LLVM-MinGW，避免目标机安装 VC++ 运行库。产物应只依赖 Windows 10 UCRT API Set 和 `KERNEL32.dll`。

## 阶段 4：迁移配置（最容易出事）

- 只迁移需要的子目录：`settings.json`、`CLAUDE.md`、`agents`、`plugins`、`skills`。
- **排除**：`.env` 中的密钥、sessions、backups、machineID/userID、日志。
- 逐文件 SHA-256 比对，报告缺失、多余、不一致三类计数，目标为 0/0/0。
- 目标目录中不能有符号链接/重解析点，否则移动后会指向宿主。
- 凭据快照放到包外隔离目录，只记录路径，不记录值。
- 迁移完成后扫描：`sk-ant-`、`ghp_`、`AKIA`、`BEGIN PRIVATE KEY`、绝对路径、用户名。

提供的环境模板：`assets/config/portable.env.template`。它只使用 `${PORTABLE_ROOT}` 和 Windows 基础变量，可直接复制后按需修改。

## 阶段 5：验收（不可跳过）

按 `references/acceptance-checklist.md` 逐项执行，至少覆盖：

1. **替身测试**：用 `mock_claude.c` 编译的假 `claude.exe`，验证参数引号、cwd、stdio、退出码、环境白名单、权限覆盖拒绝。
2. **隔离测试**：在只含 System32 的 PATH 下运行，确认不回退到宿主工具；确认宿主 `BASH_ENV`、`GIT_CONFIG_*`、`PIP_INDEX_URL`、代理变量不继承。
3. **移动测试**：复制到含空格和中文的新路径，隐藏原 Python 基座，确认 node/bash/python/pip/Chromium 仍可用。
4. **真实测试**：用真实账号跑一次非交互调用，检查 `init` 事件中的 `permissionMode`、cwd、技能与工具加载。
5. **完整性审计**：文件数、哈希、无重解析点、凭据不出现在报告。

失败要如实记录；只在单机验证过，就不能写"已在全新机器验证"。

## 阶段 6：可选本地 GUI

`assets/gui` 提供一个零构建的本地 Web 界面：Node HTTP + SSE + 常驻 Claude 子进程。`assets/src/desktop.c` 是配套的一键启动器。

```powershell
# 放到便携根目录
copy assets\gui\* <PORTABLE_ROOT>\gui\ /E
copy assets\src\desktop.c <PORTABLE_ROOT>\src\
copy assets\src\desktop.rc <PORTABLE_ROOT>\src\

cd <PORTABLE_ROOT>\gui
<PORTABLE_ROOT>\runtime\node\npm.cmd install --no-audit --no-fund

# 重新运行构建脚本，会同时产出 desktop.exe
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-launcher.ps1 -Root <PORTABLE_ROOT>
```

`desktop.exe` 的行为：检测 `127.0.0.1:<port>` 是否已在监听；未监听则以 `CREATE_NO_WINDOW` 启动 `gui\server.js`，等待就绪后打开浏览器；已监听则只打开浏览器。界面左下角"停止服务"调用 `POST /api/shutdown` 结束进程，避免后台残留。

图标：

- `desktop.rc` 引用 `src\desktop.ico`；文件不存在时构建脚本自动跳过资源编译，仍产出无图标 exe。
- `scripts/extract-icon.py` 可以从**使用者自己的** `claude.exe` 提取全部图标尺寸，需要 `pip install pefile`。
- Claude 图标属于第三方标识。提取出的 `desktop.ico` 只用于本地构建，**不要提交到公开仓库**；`.gitignore` 已排除。

关键协议与实现坑见 `references/gui-bridge.md`。GUI 默认通过 `scc.exe` 启动，因而继承同样的环境隔离与权限模式。

## 常见坑

| 现象 | 原因 | 处理 |
|---|---|---|
| 移动后 Python 找不到基座 | venv 的 `pyvenv.cfg` 写死绝对路径 | 启动器每次按 `ACTIVE_PYTHON` 重写 |
| 宿主 Node/Git 被误用 | PATH 继承或 `BASH_ENV` 注入 | 白名单环境 + 清宿主注入变量 |
| 参数含空格/引号被拆开 | 命令行拼接未按 Windows 引号规则 | 使用 `CommandLineToArgvW` 逆规则转义 |
| `--permission-mode` 被覆盖 | 直接透传用户参数 | 启动器显式拒绝该参数 |
| 技能移动后失效 | 复制了符号链接而非实体 | 迁移后检查重解析点 |
| 凭据进入报告 | 测试脚本打印环境变量 | 只记录存在性/哈希，不打印值 |
| GUI 弹窗关不掉 | CSS `display:flex` 覆盖 `[hidden]` | 加 `[hidden]{display:none!important}` |
| GUI 用户消息不显示 | 只处理了 assistant 流，漏了 `user_message` 事件 | 在 SSE 处理器中显式处理 |

## 交付物

- 可运行的 `<PORTABLE_ROOT>`（凭据与二进制不进版本库）
- `src/`、`tools/`/`scripts/`、脱敏配置模板
- 验收报告：逐项"基准 / 实际 / 结论"
- 回退说明：快照位置、凭据恢复方式
- 已知边界：未实机验证项、外部依赖（MATLAB、Abaqus、TwinCAT、网络服务等）

## 参考文件

- `references/launcher-design.md` — 启动器机制与代码走读
- `references/environment-matrix.md` — 变量、白名单、展开规则
- `references/acceptance-checklist.md` — 逐项验收清单
- `references/gui-bridge.md` — stream-json 协议与 GUI 架构
- `references/licensing-and-redistribution.md` — 分发边界与许可证
