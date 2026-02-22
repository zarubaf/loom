#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# fetch_deps.sh — Clone Snitch RTL dependencies and fetch RISC-V toolchain
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
THIRD_PARTY="${SCRIPT_DIR}/../../third_party/snitch"
TOOLCHAIN_DIR="${SCRIPT_DIR}/../../third_party/riscv-toolchain"

mkdir -p "$THIRD_PARTY" "$TOOLCHAIN_DIR"

# ── Helper ──────────────────────────────────────────────────────────────────
clone_or_update() {
    local repo=$1 tag=$2 dir=$3
    if [ -d "$dir/.git" ]; then
        echo "[deps] $dir already exists, skipping"
    else
        echo "[deps] Cloning $repo @ $tag → $dir"
        git clone --depth 1 --branch "$tag" "https://github.com/$repo.git" "$dir"
    fi
}

# ── RTL Dependencies ────────────────────────────────────────────────────────

# Snitch cluster repo (contains snitch core, reqrsp_interface)
clone_or_update "pulp-platform/snitch_cluster" main "$THIRD_PARTY/snitch_cluster"

# Common cells (fifo_v3, stream_fifo, id_queue, cf_math_pkg, etc.)
# Use "snitch" branch as referenced by snitch_cluster's Bender.yml
clone_or_update "pulp-platform/common_cells" snitch "$THIRD_PARTY/common_cells"

# AXI (axi_pkg types referenced by reqrsp_pkg)
clone_or_update "pulp-platform/axi" v0.39.0-beta.4 "$THIRD_PARTY/axi"

# CVFPU (fpnew_pkg types referenced in snitch ports even with FP_EN=0)
# Use pulp-platform fork (has fmt_mode_t used by snitch)
clone_or_update "pulp-platform/cvfpu" pulp-v0.2.3 "$THIRD_PARTY/cvfpu"

# RISC-V Debug Module (dm_pkg referenced by snitch_pkg)
clone_or_update "pulp-platform/riscv-dbg" v0.8.1 "$THIRD_PARTY/riscv-dbg"

# Tech cells generic (tc_sram_impl)
clone_or_update "pulp-platform/tech_cells_generic" v0.2.13 "$THIRD_PARTY/tech_cells_generic"

# ── RISC-V Toolchain (xPack riscv-none-elf-gcc) ──────────────────────────
# Check for either riscv-none-elf-gcc or riscv32-unknown-elf-gcc
RISCV_GCC="$TOOLCHAIN_DIR/bin/riscv-none-elf-gcc"
RISCV_GCC_ALT="$TOOLCHAIN_DIR/bin/riscv32-unknown-elf-gcc"
if [ -x "$RISCV_GCC" ] || [ -x "$RISCV_GCC_ALT" ]; then
    echo "[toolchain] RISC-V toolchain already installed at $TOOLCHAIN_DIR"
else
    OS="$(uname -s)"
    ARCH="$(uname -m)"
    TC_VERSION="14.2.0-3"

    case "$OS-$ARCH" in
        Darwin-arm64)
            TC_ARCHIVE="xpack-riscv-none-elf-gcc-${TC_VERSION}-darwin-arm64.tar.gz"
            ;;
        Darwin-x86_64)
            TC_ARCHIVE="xpack-riscv-none-elf-gcc-${TC_VERSION}-darwin-x64.tar.gz"
            ;;
        Linux-x86_64)
            TC_ARCHIVE="xpack-riscv-none-elf-gcc-${TC_VERSION}-linux-x64.tar.gz"
            ;;
        Linux-aarch64)
            TC_ARCHIVE="xpack-riscv-none-elf-gcc-${TC_VERSION}-linux-arm64.tar.gz"
            ;;
        *)
            echo "[toolchain] Unsupported platform $OS-$ARCH — install riscv-none-elf-gcc manually"
            exit 0
            ;;
    esac

    TC_URL="https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${TC_VERSION}/${TC_ARCHIVE}"
    echo "[toolchain] Downloading $TC_URL"
    curl -fsSL "$TC_URL" -o "$TOOLCHAIN_DIR/$TC_ARCHIVE"

    echo "[toolchain] Extracting to $TOOLCHAIN_DIR"
    tar xzf "$TOOLCHAIN_DIR/$TC_ARCHIVE" -C "$TOOLCHAIN_DIR" --strip-components=1

    rm -f "$TOOLCHAIN_DIR/$TC_ARCHIVE"
    echo "[toolchain] Installed: $($RISCV_GCC --version | head -1)"
fi

echo ""
echo "[done] All dependencies fetched."
echo "  RTL:       $THIRD_PARTY"
echo "  Toolchain: $TOOLCHAIN_DIR"
