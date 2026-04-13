# Termux (Android) packaging for hermes C++ port

This directory ships a build + install flow for running `hermes_cpp` on
Android under [Termux](https://termux.dev). Termux provides a Bionic-libc
userland in `/data/data/com.termux/files/usr` (`$PREFIX`). The hermes
port compiles cleanly under Termux's clang and links against Bionic —
**no glibc dependency** — so the resulting binary is self-contained for
the architecture it was built on (typically `aarch64`).

## Prerequisites

Install from the Termux app (F-Droid build strongly recommended — the
Play Store build is frozen on an old package set):

```bash
pkg update && pkg upgrade -y
pkg install -y cmake clang ninja git pkg-config \
               openssl libcurl libssh2 \
               sqlite libyaml yaml-cpp \
               boost libuv zlib
# Optional — unlock voice / Matrix E2EE:
pkg install -y libopus libolm
# Optional — Matrix decryption:
pkg install -y libsodium
```

## Build

```bash
git clone https://github.com/NousResearch/hermes-agent.git ~/hermes
cd ~/hermes
bash cpp/packaging/termux/build.sh
```

Output: `cpp/build/termux/cli/hermes_cpp`.

Useful env vars:

| Variable            | Default          | Notes                                   |
|---------------------|------------------|-----------------------------------------|
| `BUILD_DIR`         | `build/termux`   | Out-of-tree build directory             |
| `BUILD_TYPE`        | `Release`        | Debug / RelWithDebInfo also supported   |
| `JOBS`              | `$(nproc)`       | Parallel job count                      |
| `HERMES_RUN_TESTS`  | `0`              | Set `=1` to run the Termux-safe subset  |

## Install

```bash
bash cpp/packaging/termux/termux-install.sh
hermes --version
```

The installer drops:

- `$PREFIX/bin/hermes` — the binary
- `$PREFIX/share/hermes/assets/*.md` — default SOUL / BOOT templates
- `$PREFIX/share/hermes/builtins/**/SKILL.md` — 78 built-in skills
- `~/.hermes/...` — per-user state, created empty on first install

## `apt`-style limitations

Termux is *not* a Linux distro — it's an app package with a Bionic-libc
userland. Expect these differences vs. a real distro install:

- **No systemd, no launchd, no init.** The `hermes-gateway.service`
  unit has no analog. To keep the gateway alive:
  - `pkg install termux-services`, then
    `sv-enable hermes-gateway` (write your own run script, or use
    `tmux new-session -d -s hermes 'hermes gateway'`).
  - Call `termux-wake-lock` before long-running sessions so Android
    doesn't suspend the process.
- **`$PREFIX`, not `/usr`.** Every path is rebased:
  `/usr/bin/...` → `/data/data/com.termux/files/usr/bin/...`. The
  build script sets `CMAKE_INSTALL_PREFIX=$PREFIX`; `get_all_skills_dirs()`
  detects `$PREFIX` at runtime and probes `$PREFIX/share/hermes/...`.
- **Bionic, not glibc.** A few libc extensions (`execvpe`, `pthread_cancel`,
  `glibc`-style NSS, `locale.h` extensions) don't exist. Code that needs
  them is guarded by `#ifdef __ANDROID__`. If you hit a link failure on a
  symbol, add the guard; don't try to backport glibc.
- **Limited PTY support.** Several tests (`CancelFn`, `ForegroundTimeout`,
  `CopyPaste`, `PTY`) can hang on the weaker PTY layer. They are excluded
  by the ctest regex in `build.sh`.
- **`/proc` is partially redacted.** `/proc/<pid>/...` works for the
  calling process only. The gateway status introspection uses
  `/proc/self/...` exclusively for this reason.
- **Package names differ** from Debian/Ubuntu. `libcurl4-openssl-dev`
  is just `libcurl` here; `yaml-cpp-dev` is `yaml-cpp`.
- **SELinux sandboxing.** Some syscalls (`ptrace`, raw sockets, cap\_*)
  are blocked. Nothing in hermes relies on them, but plugins that shell
  out to `strace` / `tcpdump` won't work.

## Uninstall

```bash
rm -f $PREFIX/bin/hermes
rm -rf $PREFIX/share/hermes
# Leave ~/.hermes alone unless you want to wipe sessions/plans/skills.
```

## Troubleshooting

- `cmake: not found` after `pkg install cmake` — restart the Termux
  shell; `$PATH` wasn't rebuilt.
- `std::filesystem` link errors — very old Termux on ancient devices may
  ship clang 10; `pkg upgrade` should pull clang 17+.
- `OPENSSL_ROOT_DIR` misdetection — pass `-DOPENSSL_ROOT_DIR=$PREFIX`
  to `cmake` explicitly (the build script already does this).
- `hermes gateway` dies on screen-off — you forgot `termux-wake-lock`.
