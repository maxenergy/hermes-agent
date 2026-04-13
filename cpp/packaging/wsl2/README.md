# WSL2 (Windows Subsystem for Linux 2) compatibility

The C++ port of hermes-agent builds and runs unmodified on WSL2 — it is
a real Linux environment with an actual kernel, so the standard Linux
build flow works end-to-end. No Windows-specific code changes are
required to use the Linux binary on Windows via WSL2; the build is
identical to a native Ubuntu/Debian install.

The CI job `build-wsl2` in `.github/workflows/cpp-ci.yml` exercises this
path on `windows-2022` runners using `Vampire/setup-wsl@v3` with
Ubuntu-24.04 — the same distro most users run.

## Installation

### 1. Install WSL2 + Ubuntu (one-time host setup)

From an elevated PowerShell:

```powershell
wsl --install -d Ubuntu-24.04
wsl --set-default-version 2
wsl --update
```

Reboot if prompted. On first launch WSL asks for a UNIX username/password.

### 2. Install build deps inside WSL

```bash
sudo apt update
sudo apt install -y cmake ninja-build clang g++ git pkg-config \
    libssl-dev libcurl4-openssl-dev libssh2-1-dev \
    libsqlite3-dev libsqlitecpp-dev libyaml-cpp-dev \
    libboost-all-dev libfmt-dev libspdlog-dev nlohmann-json3-dev \
    libopus-dev libolm-dev libsodium-dev
```

### 3. Clone + build

```bash
git clone https://github.com/NousResearch/hermes-agent.git ~/hermes
cd ~/hermes
cmake -S cpp -B cpp/build/default -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build/default --parallel
```

### 4. Test

```bash
ctest --test-dir cpp/build/default \
    --exclude-regex "CancelFn|ForegroundTimeout|CopyPaste|SkillsHub|Live|IMAP_TEST|SINGULARITY_TEST"
```

The `WSL2_TEST` pattern is reserved for WSL2-specific regression tests
that are currently gated out of CI but passable locally; see "Tests"
below.

## Known gotchas

### Windows-binary execution (`/proc/sys/fs/binfmt_misc/WSLInterop`)

WSL2 registers a binfmt handler that lets the Linux kernel exec `.exe`
files through the Windows host. This is *usually* harmless but can
surface as confusing behavior:

- Any `.exe` anywhere on `$PATH` silently gets invoked as a Windows
  process. If you run `hermes` and hit a mounted Windows `git.exe`
  before the Linux `git`, hermes' git plumbing calls out to Windows
  git, which has CRLF / line-ending semantics that differ from Linux.
- Recommended: prepend `/usr/local/bin:/usr/bin:/bin` to `$PATH` before
  the Windows-mounted `/mnt/c/...` paths (most distros do this already
  via `/etc/wsl.conf`: `[interop] appendWindowsPath=false`).
- If you need to turn binfmt off wholesale:
  ```bash
  sudo sh -c 'echo 0 > /proc/sys/fs/binfmt_misc/WSLInterop'
  ```

### `/mnt/c` case-insensitivity

Windows drives are mounted case-insensitive by default. If you build
hermes inside `/mnt/c/...`, filename-sensitive code paths (e.g. skill
discovery matching `SKILL.md` vs. `skill.md`) can behave unexpectedly.
**Build inside the Linux filesystem** (`~/hermes`, not
`/mnt/c/Users/...`); it's also ~10× faster because NTFS
passthrough is slow for many small files.

### inotify / file watchers

WSL2 supports inotify with the same semantics as native Linux.
hermes-agent does not currently use inotify (verified by grep), so this
is listed for future reference only. If you add an inotify-based
watcher, note that `/mnt/c/...` does **not** emit inotify events when
files are modified from Windows — the events come from `fsnotify` on
the Linux kernel only.

### Signal handling

Signals behave like native Linux. The gateway's `SIGTERM` / `SIGINT`
graceful-shutdown paths work identically. WSL1 had broken signal
semantics; WSL2 does not.

### Clock skew on suspend/resume

When Windows suspends, the WSL2 VM's clock may drift. `hwclock --hctosys`
isn't available but `sudo ntpdate -b time.google.com` (or enabling
`systemd-timesyncd`) fixes it. Symptoms: TLS handshake failures with
"certificate not yet valid" after laptop wake.

### Docker Desktop integration

If Docker Desktop's WSL integration is enabled, `docker` and `docker
buildx` live in the Windows host's PATH, not inside WSL. Docker
commands from WSL hit the Windows daemon. This means
`cpp/packaging/docker-buildx.sh` works inside WSL, but the OCI archive
lands on the Windows-side filesystem unless you `cd ~/...` first.

### Port forwarding

Gateway bind to `0.0.0.0:<port>` inside WSL2 → Windows auto-forwards
from `localhost:<port>` on newer versions (22H2+). On older versions
you may need `netsh interface portproxy add` rules. See Microsoft's
WSL networking docs for the canonical incantation.

## Tests

WSL2-specific regression tests are tagged with the `WSL2_TEST` prefix so
they can be gated via ctest:

```bash
# Include WSL2 tests explicitly (still gated out of the default suite):
ctest --test-dir cpp/build/default --tests-regex "WSL2_TEST"
```

The project's default exclusion regex (shown in `CLAUDE.md` and CI) does
**not** exclude `WSL2_TEST` — the exclusion list in the task brief is a
superset used by some agents. Local runs don't need special flags unless
you want to skip them on WSL1 or native Linux.

## When not to use WSL2

- **Real-time audio/video** — WSLg passthrough has latency spikes.
- **GPU training** — works with CUDA-WSL, but expect 5-10% throughput
  penalty vs. bare-metal Linux.
- **Low-level kernel tooling** — WSL2 ships a custom kernel; `eBPF`
  support is partial, `perf` can be flaky.

For everything else — CLI, gateway, tests, packaging — WSL2 is a
first-class target.
