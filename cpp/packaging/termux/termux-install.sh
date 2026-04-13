#!/data/data/com.termux/files/usr/bin/env bash
# cpp/packaging/termux/termux-install.sh
#
# Install a freshly-built hermes_cpp into $PREFIX/bin on Termux, drop
# the built-in skill collection under $PREFIX/share/hermes/builtins,
# and copy the default assets. Intended to be run after build.sh.
#
# No systemd / no service wrappers — Termux processes run foreground.
# If you want the gateway to survive an app kill, use termux-wake-lock
# and termux-services (pkg install termux-services) separately.

set -euo pipefail

: "${PREFIX:?Termux PREFIX not set — are you running inside Termux?}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$CPP_DIR/build/termux}"
BIN="$BUILD_DIR/cli/hermes_cpp"

if [[ ! -x "$BIN" ]]; then
    echo "[termux-install] ERROR: no binary at $BIN"
    echo "                 Run cpp/packaging/termux/build.sh first."
    exit 2
fi

DEST_BIN="$PREFIX/bin/hermes"
echo "[termux-install] Installing binary -> $DEST_BIN"
install -Dm755 "$BIN" "$DEST_BIN"

ASSETS_SRC="$CPP_DIR/assets"
ASSETS_DST="$PREFIX/share/hermes/assets"
if [[ -d "$ASSETS_SRC" ]]; then
    echo "[termux-install] Installing assets  -> $ASSETS_DST"
    mkdir -p "$ASSETS_DST"
    cp -f "$ASSETS_SRC"/*.md "$ASSETS_DST"/ 2>/dev/null || true
fi

BUILTINS_SRC="$CPP_DIR/skills/builtins"
BUILTINS_DST="$PREFIX/share/hermes/builtins"
if [[ -d "$BUILTINS_SRC" ]]; then
    echo "[termux-install] Installing builtins -> $BUILTINS_DST"
    mkdir -p "$BUILTINS_DST"
    # Copy the full directory tree preserving the category/<name>/SKILL.md
    # layout.  cp -r works fine on Termux coreutils.
    cp -rf "$BUILTINS_SRC"/. "$BUILTINS_DST"/
fi

# Create the per-user state tree if absent.
HERMES_HOME_DEFAULT="$HOME/.hermes"
mkdir -p "$HERMES_HOME_DEFAULT"/{skills,optional-skills,installed-skills,plans,logs,sessions}

echo "[termux-install] Done."
echo
echo "Try:     hermes --version"
echo "Config:  $HERMES_HOME_DEFAULT/config.yaml"
echo
echo "Caveats on Android / Termux:"
echo "  * no systemd — use \`hermes gateway\` in a tmux session or"
echo "    termux-services to keep the gateway running."
echo "  * acquire a wake-lock (\`termux-wake-lock\`) if you want the"
echo "    agent to survive screen-off."
echo "  * storage access: run \`termux-setup-storage\` once so that"
echo "    /sdcard/... paths resolve."
