#!/usr/bin/env bash
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Build kerrigan-coordinator, the AI swarm coordinator kerrigand runs on a
# BroodNode, for the Linux release archives.
#
#   contrib/coordinator/build.sh <src> <out> [x86_64-linux-gnu] [aarch64-linux-gnu]
#
# <src> holds kerrigan-swarm/ and kerrigan-sdk/ side by side (the swarm
# workspace path-depends on ../kerrigan-sdk/sdk) and, optionally, a SOURCES
# file naming the commits, which is copied next to the binaries. Each binary
# lands in <out>/<host>/kerrigan-coordinator, where build-release.sh picks it
# up (COORDINATOR_DIR=<out>).
#
# The binaries link libstdc++ and libgcc statically, so their only runtime
# dependency is glibc, and build paths are remapped so no host path is
# embedded. Rust comes from rustup, pinned to RUST_TOOLCHAIN.
set -euo pipefail

RUST_TOOLCHAIN="${RUST_TOOLCHAIN:-1.94.1}"
SRC="$(cd "${1:?usage: build.sh <src> <out> [hosts...]}" && pwd)"
OUT="$(mkdir -p "${2:?usage: build.sh <src> <out> [hosts...]}" && cd "$2" && pwd)"
shift 2
HOSTS=("$@")
[ ${#HOSTS[@]} -gt 0 ] || HOSTS=(x86_64-linux-gnu aarch64-linux-gnu)

export RUSTUP_HOME="${RUSTUP_HOME:-$HOME/.rustup}" CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"
RUSTUP="$CARGO_HOME/bin/rustup"
[ -x "$RUSTUP" ] || { echo "rustup not found at $RUSTUP (install it, then: rustup toolchain install $RUST_TOOLCHAIN)" >&2; exit 1; }
CARGO=("$RUSTUP" run "$RUST_TOOLCHAIN" cargo)

WORKSPACE="$SRC/kerrigan-swarm"
[ -f "$WORKSPACE/Cargo.toml" ] && [ -d "$SRC/kerrigan-sdk/sdk" ] || { echo "$SRC must hold kerrigan-swarm/ and kerrigan-sdk/" >&2; exit 1; }

for host in "${HOSTS[@]}"; do
    case "$host" in
        x86_64-linux-gnu)  triple=x86_64-unknown-linux-gnu;  cc=gcc;                cxx=g++;                  ;;
        aarch64-linux-gnu) triple=aarch64-unknown-linux-gnu; cc=aarch64-linux-gnu-gcc; cxx=aarch64-linux-gnu-g++; ;;
        *) echo "unsupported host $host" >&2; exit 1 ;;
    esac
    "$RUSTUP" target add --toolchain "$RUST_TOOLCHAIN" "$triple" >/dev/null

    # libstdc++ and libgcc_s resolve to static archives: a search dir holding
    # only those archives sits ahead of the system dirs, so the explicit
    # -lstdc++ (zeromq) and -lgcc_s (Rust std) link statically.
    gcclib="$(dirname "$("$cxx" -print-file-name=libstdc++.a)")"
    staticlibs="$OUT/.staticlibs/$host"
    rm -rf "$staticlibs" && mkdir -p "$staticlibs"
    ln -s "$gcclib/libstdc++.a" "$staticlibs/libstdc++.a"
    printf 'create %s\naddlib %s\naddlib %s\nsave\nend\n' \
        "$staticlibs/libgcc_s.a" "$gcclib/libgcc.a" "$gcclib/libgcc_eh.a" | ar -M

    target_env="$(echo "$triple" | tr 'a-z-' 'A-Z_')"
    target_dir="$OUT/.target"
    export "CARGO_TARGET_${target_env}_LINKER=$cc"
    export "CC_${triple//-/_}=$cc" "CXX_${triple//-/_}=$cxx"
    export "CARGO_TARGET_${target_env}_RUSTFLAGS=--remap-path-prefix=$SRC=/build --remap-path-prefix=$CARGO_HOME=/cargo --remap-path-prefix=$RUSTUP_HOME=/rustup -L native=$staticlibs -C link-arg=-static-libgcc"
    export CFLAGS="-ffile-prefix-map=$SRC=/build -ffile-prefix-map=$CARGO_HOME=/cargo"
    export CXXFLAGS="$CFLAGS"

    echo "== kerrigan-coordinator for $host ($triple, rust $RUST_TOOLCHAIN)"
    (cd "$WORKSPACE" && "${CARGO[@]}" build --locked --release -p krgn-coordinator --target "$triple" --target-dir "$target_dir")
    install -D -m0755 "$target_dir/$triple/release/krgn-coordinator" "$OUT/$host/kerrigan-coordinator"
    [ -f "$SRC/SOURCES" ] && cp "$SRC/SOURCES" "$OUT/$host/kerrigan-coordinator.SOURCES"

    bin="$OUT/$host/kerrigan-coordinator"
    if objdump -p "$bin" | grep -qE 'NEEDED +lib(stdc\+\+|gcc_s)'; then
        echo "$bin still links libstdc++ or libgcc_s dynamically" >&2; exit 1
    fi
    if LC_ALL=C strings "$bin" | grep -qE '/home/[a-z]'; then
        echo "$bin embeds a /home/ path" >&2; exit 1
    fi
    echo "   $(objdump -T "$bin" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1) floor, $(du -h "$bin" | cut -f1)"
done
