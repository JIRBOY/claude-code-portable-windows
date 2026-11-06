# 启动器设计说明

`assets/src/scc.c` 是一个静态链接的 Windows 控制台程序，职责只有一件事：把当前进程改造成受控的便携环境，然后启动同目录的 `claude.exe`，并原样转发退出码。

## 启动顺序

1. `get_module_root()`：用 `GetModuleFileNameW` 取自身路径，去掉文件名得到根目录。处理盘符根（`C:\`）和超长路径。
2. 设置 `PORTABLE_ROOT`，并把它登记为"本文件已定义变量"。
3. `clear_host_environment()`：删除宿主的 Python/Node/Git/npm/Playwright 注入变量，避免宿主配置泄漏进子进程。
4. 读取 `config\portable.env`（UTF-8，上限 1 MiB，拒绝 NUL 字节和非法 UTF-8）。
5. `apply_environment_file()`：逐行解析 `NAME=VALUE`，展开 `${NAME}`，写入当前进程环境。
6. `require_environment()`：检查 19 个必需变量，缺一即失败退出。
7. `prepare_directories()`：创建 `data` 下的配置、HOME、临时、缓存、AppData 目录，并确保目录变量指向真实目录。
8. `repair_venv_paths()`：读取 `runtime\uv-python\ACTIVE_PYTHON`，原子重写 `runtime\venv\pyvenv.cfg`。
9. `validate_runtime()`：检查 `claude.exe`、node、pwsh、bash、uv、venv python 六件套。
10. `build_child_command()`：拼接命令行，强制追加权限参数，拒绝用户覆盖。
11. `build_environment_block()`：只保留基础变量 + 本文件定义变量，排序后构造环境块。
12. `CreateProcessW` + `WaitForSingleObject` + `GetExitCodeProcess`，转发退出码。

## 环境白名单

允许从宿主继承的只有 Windows 自身运行所需变量，例如：

`SystemRoot`、`WINDIR`、`SystemDrive`、`ComSpec`、`PATHEXT`、`NUMBER_OF_PROCESSORS`、`PROCESSOR_*`、`OS`、`ProgramData`、`ProgramFiles*`、`CommonProgramFiles*`、`PUBLIC`、`ALLUSERSPROFILE`、`USERNAME`、`USERDOMAIN`、`USERDNSDOMAIN`、`COMPUTERNAME`、`LOGONSERVER`、`LANG`、`LC_*`。

其余一律不继承。需要宿主能力时，只能由使用者显式写入 `portable.env`。

## 变量展开规则

- 语法：`${NAME}`，只支持这一种形式。
- 允许引用：白名单内的基础变量，或本文件中**前面已经定义**的变量。
- 拒绝：未闭合、空名、非法字符、前向引用、未定义、值为空、展开后超长。
- 失败时输出行号与原因，但**不回显变量值**，避免凭据进日志。
- 不支持 `%VAR%`、`$VAR`、命令替换、引号去壳。

## venv 自修复

标准 CPython venv 在 `pyvenv.cfg` 里写死 `home` 和 `executable` 的绝对路径。目录移动后，venv python 会指向不存在的基座。

启动器每次启动时：

1. 读 `runtime\uv-python\ACTIVE_PYTHON`，校验是安全的目录名（字母数字、`.`、`-`、`_`）。
2. 拼出 `<root>\runtime\uv-python\<版本>` 和其中的 `python.exe`。
3. 生成新的 `home=...`、`include-system-site-packages=false`、`executable=...`。
4. 内容相同则跳过；不同则写临时文件 + `FlushFileBuffers` + `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` 原子替换。

因此不要手工修改 `pyvenv.cfg` 并期待持久生效。

## 参数与权限

- 参数按 Windows 命令行规则引用：反斜杠数量、内嵌双引号、尾反斜杠都要正确加倍；支持 `--` 之后的字面参数。
- 启动器无条件追加 `--permission-mode bypassPermissions`。
- 检测到 `--permission-mode`、`--permission-mode=...`、`--dangerously-skip-permissions`、`--allow-dangerously-skip-permissions` 时直接报错退出。
- 对外分发场景应改为 `--permission-mode manual` 或接入 SDK 的权限回调，不要默认 bypass。

## 失败模式

所有失败都返回非零退出码，并在 stderr 输出 `scc: ...`。典型信息：

- `cannot read config/portable.env as UTF-8`
- `portable.env line N: referenced variable is not defined by this file`
- `portable.env is missing a required variable`
- `the active managed Python interpreter is missing`
- `required runtime component is missing (index N)`
- `permission-mode overrides are not allowed`

调试时不要打印整个环境块；用逐项存在性检查代替。

## 编译

```powershell
$clang = "<toolchain>\bin\x86_64-w64-mingw32-clang.exe"
& $clang -std=c11 -O2 -Wall -Wextra -Werror -static -municode `
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 `
  -o scc.exe src\scc.c -lshell32
```

验证：`dumpbin /imports scc.exe` 或等价工具应只显示 UCRT API Set 与 `KERNEL32.dll`，不得依赖 `VCRUNTIME140.dll`。
