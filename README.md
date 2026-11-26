> [!NOTE]
> Consolidated into the unified Holo v11 monorepo at https://github.com/awesome-os/awesome-os (all existing v11 CAS release assets remain downloadable).

# `@awesome-os/powershell` - Native Zero-`cmd.exe` Ghost PowerShell Launcher

Drop-in native C11 `pwsh.exe` / `pwsh` / `pwsh.wasm` launcher (~25 KB, zero `cmd.exe`, zero `.bat`/`.cmd` shims, zero `%*` pipe/redirect mangling) that hydrates rolling-latest PowerShell **once** from [`awesome-os/b8g-ghost`](https://github.com/awesome-os/b8g-ghost) (`cas-v1`, Keep-Last-5) into `~/.b8g/cas/pwsh/<sha256>/`.

## Quick Install & Usage

### Windows (PowerShell 1-liner)

```powershell
irm https://github.com/awesome-os/powershell/releases/latest/download/install.ps1 | iex
```

### Linux (x64 / ARM64) & macOS (Apple Silicon)

```sh
curl -fsSL https://github.com/awesome-os/powershell/releases/latest/download/install.sh | sh
```

### CLI Usage

```powershell
# Inspect installed PowerShell version & help
pwsh --version
pwsh --help

# Run a PowerShell script or inline command
pwsh -NoProfile -File ./script.ps1
pwsh -NoProfile -Command "1..5 | Where-Object { $_ -gt 2 } | Measure-Object -Sum"

# Optional: store hydrated CAS layers on a custom drive/path
$env:B8G_GHOST_CACHE_DIR = "D:\b8g-cas"   # POSIX: export B8G_GHOST_CACHE_DIR=/data/b8g-cas
pwsh --ghost-info
```

### Releases & Verification

- **Latest Release:** [`awesome-os/powershell` Releases](https://github.com/awesome-os/powershell/releases/latest) — includes `SHA256SUMS.txt` and the 5-OS matrix (`win64`, `linux-x64`, `linux-arm64`, `macos-arm64`, `wasm32-wasi`) built via [`awesome-os/maintenance-workflows`](https://github.com/awesome-os/maintenance-workflows).

## Why Native C11 + `GetCommandLineW()`?

Batch wrappers (`.cmd` / `.bat`) route through `cmd.exe` and mangle `|`, `&`, `<`, `>`, `^`, and quoted PowerShell scriptblocks via `%*` expansion.

`@awesome-os/powershell` eliminates `cmd.exe` completely:

- **Windows (`_WIN32`)**: reads the raw UTF-16 command line via `GetCommandLineW()`, strips only `argv[0]`, and spawns `<cas>\pwsh\<sha256>\pwsh.exe` via `CreateProcessW()` with the exact verbatim argument tail.
- **Linux / macOS (POSIX)**: replaces the launcher process directly via `execv()` to `<cas>/pwsh/<sha256>/pwsh`.
- **WASI (`wasm32-wasi`)**: supports `--ghost-version`, `--ghost-info`, `--help`, and local CAS manifest inspection without host process execution.

## Supported Targets

| Target | Binary | CAS Key |
| :--- | :--- | :--- |
| Windows x64 | `pwsh.exe` | `pwsh-win64.tar.zst` |
| Linux x64 (musl static) | `pwsh-linux-x64` | `pwsh-linux-x64.tar.zst` |
| Linux arm64 (musl static) | `pwsh-linux-arm64` | `pwsh-linux-arm64.tar.zst` |
| macOS arm64 | `pwsh-macos-arm64` | `pwsh-macos-arm64.tar.zst` |
| WebAssembly WASI | `pwsh.wasm` | manifest inspection / metadata |

## Usage

```powershell
# Inspect launcher version & target metadata
pwsh --ghost-version

# Inspect CAS directory, logical key, and cached sha256
pwsh --ghost-info

# Run PowerShell commands with zero cmd.exe mangling
pwsh -NoProfile -Command "1..5 | Where-Object { $_ -gt 2 } | Measure-Object -Sum"
```

## Build (`holo-compiler` / `zig cc`)

```powershell
# Win64 PE
zig cc -target x86_64-windows-gnu -std=c11 -O2 -s -Wall -Wextra src/pwsh_launcher.c -o dist/pwsh.exe

# Linux x64 static ELF64
zig cc -target x86_64-linux-musl -static -std=c11 -O2 -s -Wall -Wextra src/pwsh_launcher.c -o dist/pwsh-linux-x64

# Linux arm64 static ELF64
zig cc -target aarch64-linux-musl -static -std=c11 -O2 -s -Wall -Wextra src/pwsh_launcher.c -o dist/pwsh-linux-arm64

# macOS arm64 Mach-O
zig cc -target aarch64-macos -std=c11 -O2 -Wall -Wextra src/pwsh_launcher.c -o dist/pwsh-macos-arm64

# WASI wasm32
zig cc -target wasm32-wasi -std=c11 -Os -s -Wall -Wextra src/pwsh_launcher.c -o dist/pwsh.wasm
```

## License

MIT

## Quick Install & Run (Windows / Linux / macOS)

```sh
# Install / hydrate from v11 CAS via holo-pm
holo-pm install repository://github.com/awesome-os/powershell@main

# Or project & run directly via holo-project (0-byte hardlink)
holo-project "project://node/powershell?from=repository://github.com/awesome-os/powershell@main"
```
