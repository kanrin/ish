#!/bin/bash
# Fetch the root filesystem that gets bundled into the app as root.tar.gz.
#
# Xcode's "Download Root" build phase runs this with the usual build settings
# exported into the environment, so it reads BUILT_PRODUCTS_DIR,
# CONTENTS_FOLDER_PATH and ROOTFS_URL (defined in app/iSH.xcconfig).
#
# The original one-liner was `curl -L https://$ROOTFS_URL -o .../root.tar.gz`,
# which has two problems in practice:
#
#   * it downloads unconditionally, so a root filesystem that is already there
#     is fetched again on every build that reruns the phase;
#   * any network hiccup fails the whole build. GitHub release downloads in
#     particular are unusable on some networks ("curl: (16) Error in the HTTP2
#     framing layer", "curl: (52) Empty reply from server"), which makes the
#     app impossible to build there even though the file may already be
#     present and fine.
#
# So: reuse what is there, retry properly when fetching, and only fail when
# there is no root filesystem at all. ROOTFS_URL may also be a full URL or an
# absolute path to a file that was obtained some other way, which is the
# escape hatch for a blocked release host.
set -e

dest="$BUILT_PRODUCTS_DIR/$CONTENTS_FOLDER_PATH/root.tar.gz"
marker="$dest.url"
# A copy that survives "Clean Build Folder" (and Xcode deleting DerivedData):
# without it, a build with no network to the release host cannot produce the
# root filesystem at all. build/ is gitignored.
cache_dir="$SRCROOT/build/rootfs-cache"
cache="$cache_dir/root.tar.gz"
cache_marker="$cache.url"

case "$ROOTFS_URL" in
    *://*) url="$ROOTFS_URL" ;;        # 已经带 scheme 的完整 URL
    /*)    url="file://$ROOTFS_URL" ;; # 本地绝对路径
    *)     url="https://$ROOTFS_URL" ;; # iSH.xcconfig 里写的是裸域名/路径
esac

mkdir -p "$(dirname "$dest")"

if [ -s "$dest" ] && [ "$(cat "$marker" 2>/dev/null)" = "$url" ]; then
    echo "root.tar.gz is already up to date ($url)"
    exit 0
fi

if [ -s "$cache" ] && [ "$(cat "$cache_marker" 2>/dev/null)" = "$url" ]; then
    echo "Reusing the cached root.tar.gz ($(du -h "$cache" | cut -f1), $url)"
    cp "$cache" "$dest"
    printf '%s' "$url" > "$marker"
    exit 0
fi

echo "Downloading $url"
rm -f "$dest.url"

# The transfer options only make sense for http(s): a local file is just
# copied, and retrying that would only waste time.
transfer_opts=(--fail --location)
resume_opts=()
case "$url" in
    http://*|https://*)
        transfer_opts+=(--http1.1 --retry 3 --retry-all-errors --retry-delay 2
                        --connect-timeout 15 --max-time 1800)
        resume_opts=(-C -)
        ;;
esac

ok=0
# Keep a partial file for the next attempt and try to resume it; if the server
# does not support ranges, or this is a fresh attempt, start over.
if [ -s "$dest.part" ] && [ "${#resume_opts[@]}" -gt 0 ]; then
    curl "${transfer_opts[@]}" "${resume_opts[@]}" -o "$dest.part" "$url" && ok=1 || true
fi
if [ "$ok" = 0 ]; then
    curl "${transfer_opts[@]}" -o "$dest.part" "$url" && ok=1 || true
fi

if [ "$ok" = 1 ]; then
    mv "$dest.part" "$dest"
    printf '%s' "$url" > "$marker"
    mkdir -p "$cache_dir"
    cp "$dest" "$cache"
    printf '%s' "$url" > "$cache_marker"
    echo "Saved $(du -h "$dest" | cut -f1) to $dest (and cached it for clean builds)"
elif [ -s "$dest" ]; then
    # Keep a usable root filesystem rather than breaking the build over a
    # download that this network cannot make.
    echo "warning: could not download $url" >&2
    echo "warning: keeping the existing $(du -h "$dest" | cut -f1) root.tar.gz" >&2
    echo "warning: note it down, because it is not retried until it is removed:" >&2
    echo "warning: a clean build (or deleting $dest) downloads again" >&2
    printf '%s' "$url" > "$marker"
else
    echo "error: could not download the root filesystem from $url" >&2
    echo "note: if this network cannot reach the release host, download it" >&2
    echo "      elsewhere and point ROOTFS_URL in app/iSH.xcconfig at the file," >&2
    echo "      e.g. ROOTFS_URL = /Users/you/appstore-apk.tar.gz" >&2
    exit 1
fi
