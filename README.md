# Claude Code Portable (Windows) — Agent Skill

A reproducible method for building a self-contained, relocatable Claude Code installation on Windows 10/11 x64, plus a dependency-free local web GUI.

## What's inside

- `SKILL.md` — the workflow: runtime assembly, native launcher, config migration, acceptance, optional GUI
- `assets/src/scc.c` — environment-isolating native launcher (C11, static)
- `assets/src/tool_shim.c` — path-independent `pip`/`pytest`/`playwright` shims
- `assets/src/mock_claude.c` — test double for launcher behavior tests
- `assets/config/portable.env.template` — credential-free environment template
- `assets/gui/` — local web UI (Node + SSE + stream-json)
- `scripts/` — fixed-SHA toolchain download and launcher build
- `references/` — launcher internals, environment matrix, acceptance checklist, GUI protocol, licensing

## What's NOT inside

- `claude.exe` (proprietary, obtain it from your own official installation)
- credentials, sessions, account data
- bundled runtimes (Node, Git, PowerShell, Python, Chromium)
- third-party skills or plugins

## Quick start

```powershell
# 1. Lay out the portable root and copy your official claude.exe into it
# 2. Get the compiler toolchain (fixed SHA-256)
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\get-toolchain.ps1
# 3. Build the launcher
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-launcher.ps1 -Root <PORTABLE_ROOT>
```

See `SKILL.md` for the full staged workflow and `references/acceptance-checklist.md` before claiming completion.

## Optional GUI

```powershell
copy assets\gui\* <PORTABLE_ROOT>\gui\ /E
copy assets\src\desktop.c <PORTABLE_ROOT>\src\
copy assets\src\desktop.rc <PORTABLE_ROOT>\src\
cd <PORTABLE_ROOT>\gui
<PORTABLE_ROOT>\runtime\node\npm.cmd install --no-audit --no-fund
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-launcher.ps1 -Root <PORTABLE_ROOT>
```

Double-click `desktop.exe` to start the local server hidden and open the browser. Stop it from the GUI's "停止服务" button. The icon is optional: extract it from your own `claude.exe` with `scripts/extract-icon.py`; extracted icon files are ignored by git and not shipped here.

## License

MIT. Third-party runtimes and Claude Code itself are not covered by this license.
