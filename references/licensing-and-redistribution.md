# 分发边界与许可证

这份技能只包含**方法、原创启动器源码和脱敏模板**。它不是便携包本身。

## 不随本技能分发

| 内容 | 原因 |
|---|---|
| `claude.exe` | Anthropic 的专有二进制，授权不允许再分发 |
| `data\.claude\` 全部内容 | 含登录令牌、会话、可能含第三方技能 |
| `runtime\` | 体积巨大，且各组件有各自的许可证与再分发条件 |
| `.snapshots\`、测试副本 | 可能含凭据或用户数据 |
| 第三方技能、插件、市场缓存 | 授权各不相同，需逐个确认 |
| 任何 `ANTHROPIC_AUTH_TOKEN`、第三方 API 地址 | 凭据与账号安全 |

## 使用者需要自行获取

- Claude Code 官方程序：从自己的官方安装中取得 `claude.exe`。
- Node.js：官方 Windows x64 包。
- Git for Windows：官方安装包。
- PowerShell 7：官方 release 包。
- uv 与 CPython：通过 uv 官方渠道安装。
- Playwright Chromium：通过 Playwright 官方渠道安装。

各组件遵循自己的许可证。再分发运行时前，必须逐一核对：Node（MIT 及其依赖许可）、Git for Windows（GPL 及其他）、PowerShell（MIT）、CPython（PSF）、Chromium（BSD 等）。本技能的 MIT 许可证不覆盖这些组件。

## 凭据处理

- 不把令牌写入源码、配置模板、技能说明、测试报告或版本库。
- 扫描关键词：`sk-ant-`、`ghp_`、`gho_`、`AKIA`、`-----BEGIN ... PRIVATE KEY-----`、`ANTHROPIC_AUTH_TOKEN`、`ANTHROPIC_BASE_URL`。
- 日志只记录键名、是否存在、长度或哈希。
- 迁移时如需临时快照，放到包外隔离目录，并记录路径而不是内容。
- 不再需要时，删除目录之外还应轮换令牌。

## 对外分发时额外要做

1. 把启动器的权限模式从 `bypassPermissions` 改为 `manual`，或接入 SDK 权限回调。
2. 提供首次运行向导，让使用者填入自己的凭据，而不是预置。
3. 明确"运行时由使用者自行安装"，不要打包成一体包上传。
4. 在 README 中列出所有第三方组件的来源与许可证。
5. 增加卸载说明：删除目录、清理计划任务/快捷方式、轮换令牌。

## 本技能的许可证

MIT。可以修改、再分发、商用，但需保留版权声明。启动器源码为原创实现；如果参考了其他项目，应在其文件头注明来源与许可证。
