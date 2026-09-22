#!/bin/bash
# Restore the git submodules that the Xcode build needs.
#
#   deps/libarchive  - the sources of the "libarchive" target. Without them
#                      Xcode reports "Build input file cannot be found:
#                      .../deps/libarchive/libarchive/xxx.c".
#   deps/libapps     - provides deps/libapps/hterm/bin/mkdist, which the
#                      "Compile JavaScript" build phase runs.
#
# Neither is needed for the command line build (meson build / ninja), so this
# only matters when building the app from Xcode.
#
# deps/linux is deliberately not restored: .gitmodules marks it "update = none"
# and only the alternative kernel=linux build needs it.
#
# Safe to re-run: existing checkouts are left alone. A plain
# `git submodule update --init` clones the whole history, which can take
# forever on a throttled connection, so this does a shallow clone and then
# fetches exactly the commit the superproject pins -- which is all a build
# needs. If deps/libarchive has been deinitialised (git submodule status shows
# it with a leading '-'), running this script puts it back in a minute or two.
set -e
cd "$(dirname "$0")/.."

restore() {
    local path="$1"
    local rev
    rev=$(git ls-tree HEAD "$path" | awk '{print $3}')
    if [ -z "$rev" ]; then
        echo "==> $path: 找不到 Gitlink，跳过" >&2
        return
    fi
    echo "==> $path @ ${rev:0:12}"
    if [ -n "$(ls -A "$path" 2>/dev/null)" ]; then
        echo "    已有内容，跳过"
        return
    fi
    git submodule update --init --depth 1 "$path" || true
    if ! git -C "$path" cat-file -e "${rev}^{commit}" 2>/dev/null; then
        echo "    浅克隆里没有这个提交，单独取回"
        git -C "$path" fetch --depth 1 origin "$rev"
    fi
    git -C "$path" checkout -q "$rev"
    echo "    完成：$(ls -A "$path" | wc -l | tr -d ' ') 项"
}

restore deps/libarchive
restore deps/libapps

echo
git submodule status
