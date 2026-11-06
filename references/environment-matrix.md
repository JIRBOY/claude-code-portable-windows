# 环境变量矩阵

完整模板见 `assets/config/portable.env.template`。所有路径必须写成 `${PORTABLE_ROOT}\...`，不得出现盘符或用户名。

## 必需变量

启动器会逐项检查，缺失即退出。

| 变量 | 作用 |
|---|---|
| `PATH` | 只含便携运行时 + `System32` |
| `CLAUDE_CONFIG_DIR` | 指向 `data\.claude` |
| `HOME`、`USERPROFILE` | 指向 `data\home` |
| `APPDATA`、`LOCALAPPDATA` | 指向 `data\AppData\...` |
| `TEMP`、`TMP` | 指向 `data\tmp` |
| `VIRTUAL_ENV`、`VIRTUAL_ENV_PROMPT` | 指向 `runtime\venv` |
| `UV_PYTHON_INSTALL_DIR` | 指向 `runtime\uv-python` |
| `UV_PROJECT_ENVIRONMENT` | 指向 `runtime\venv` |
| `UV_CACHE_DIR`、`PIP_CACHE_DIR` | 指向 `data\cache` |
| `PLAYWRIGHT_BROWSERS_PATH` | 指向 `runtime\ms-playwright` |
| `NPM_CONFIG_CACHE`、`NPM_CONFIG_USERCONFIG` | 指向 `data\cache` / `data\home\.npmrc` |
| `CLAUDE_CODE_GIT_BASH_PATH` | 指向便携 `bash.exe` |
| `CLAUDE_CODE_USE_POWERSHELL_TOOL` | `1`，启用 PowerShell 工具 |

## 推荐变量

| 变量 | 值 | 原因 |
|---|---|---|
| `DISABLE_AUTOUPDATER` | `1` | 防止运行中改写便携目录 |
| `PYTHONNOUSERSITE` | `1` | 不加载宿主用户 site-packages |
| `PYTHONUTF8` | `1` | 统一 UTF-8，减少中文乱码 |
| `PYTHONDONTWRITEBYTECODE` | `1` | 不在包内散落 `__pycache__` |
| `PIP_CONFIG_FILE` | `${PORTABLE_ROOT}\config\pip.ini` | 固定 pip 行为 |
| `UV_NO_CONFIG` | `1` | 不读宿主 uv 配置 |
| `UV_LINK_MODE` | `copy` | 避免跨盘硬链接失败 |
| `NPM_CONFIG_UPDATE_NOTIFIER` | `false` | 减少无关网络请求 |
| `POWERSHELL_TELEMETRY_OPTOUT` | `1` | 关闭遥测 |
| `DOTNET_CLI_TELEMETRY_OPTOUT` | `1` | 关闭遥测 |
| `GIT_CONFIG_GLOBAL` | `data\home\.gitconfig` | Git 配置留在包内 |
| `MSYSTEM` | `MINGW64` | Bash 工具的行为一致性 |
| `CHERE_INVOKING` | `1` | 让 Git Bash 保留当前目录 |

## `PATH` 的顺序

```text
runtime\bin
runtime\venv\Scripts
runtime\uv
runtime\node
runtime\pwsh
runtime\git\cmd
runtime\git\bin
runtime\git\usr\bin
%SystemRoot%\System32
%SystemRoot%
```

先放 `runtime\bin`，让 `pip`、`pytest`、`playwright`、`markitdown` 命中便携 shim；`System32` 放最后，保证 `cmd`、`where`、`taskkill` 仍可用。

## 展开与安全

- `${NAME}` 只能是白名单变量或本文件前文已定义变量。
- 前向引用是刻意禁止的：它让配置的依赖顺序可见、可审查。
- 不打印变量值；日志只记录键名、是否存在、长度或哈希。
- `ANTHROPIC_AUTH_TOKEN`、`ANTHROPIC_BASE_URL` 不写进 `portable.env`，放在 `data\.claude\settings.json` 或 `.env`，并由使用者自行保管。

## 宿主隔离检查

在启动器进程内验证以下变量为空或不存在：

`PYTHONHOME`、`PYTHONPATH`、`NODE_PATH`、`NODE_OPTIONS`、`GIT_DIR`、`GIT_WORK_TREE`、`GIT_CONFIG_GLOBAL`（宿主值）、`VIRTUAL_ENV`（宿主值）、`BASH_ENV`、`PIP_INDEX_URL`、`HTTP_PROXY`、`HTTPS_PROXY`、`AWS_*`、`ANTHROPIC_*`。

需要代理或镜像时，由使用者显式写入 `portable.env`，并记录这是用户扩展、不属于自包含范围。
