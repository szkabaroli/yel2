#!/bin/sh
# Installs yel (yelc and yel-lsp, with the runtime they read) from a GitHub release:
#
#   curl -fsSL https://raw.githubusercontent.com/szkabaroli/yel2/main/install.sh | sh
#
# YEL_VERSION   the release's tag (pre-alpha-3); by default the newest, pre-releases included
# YEL_INSTALL   where it goes (by default ~/.yel): the archive's directory, bin/ in it
#
# Linux and macOS, x64 and arm64. A C compiler (cc) builds what yelc makes.
set -eu

repo=szkabaroli/yel2
dir=${YEL_INSTALL:-"$HOME/.yel"}

fail() { echo "yel: $*" >&2; exit 1; }
need() { command -v "$1" > /dev/null 2>&1 || fail "needs $1"; }
need curl
need tar

case "$(uname -s)" in
Darwin) os=darwin ;;
Linux) os=linux ;;
*) fail "no release for $(uname -s) yet (Linux and macOS): build from source, https://github.com/$repo#building-from-source" ;;
esac
case "$(uname -m)" in
x86_64 | amd64) cpu=x64 ;;
arm64 | aarch64) cpu=arm64 ;;
*) fail "no release for $(uname -m) yet (x64 and arm64)" ;;
esac
# (an x64 shell under Rosetta on an arm64 Mac: the arm64 build)
if [ "$os-$cpu" = darwin-x64 ] && [ "$(sysctl -n sysctl.proc_translated 2> /dev/null || echo 0)" = 1 ]; then cpu=arm64; fi
target="$os-$cpu"

# the newest release, pre-releases included (GitHub's latest is the newest stable one only)
tag=${YEL_VERSION:-}
if [ -z "$tag" ]; then
	tag=$(curl -fsSL "https://api.github.com/repos/$repo/releases?per_page=1" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)
	[ -n "$tag" ] || fail "found no release of $repo"
fi

url="https://github.com/$repo/releases/download/$tag/yel-$target.tar.gz"
echo "yel: $tag for $target"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fSL --progress-bar "$url" -o "$work/yel.tar.gz" || fail "could not download $url"
tar -xzf "$work/yel.tar.gz" -C "$work"
[ -x "$work/yel-$target/bin/yelc" ] || fail "the archive has no bin/yelc"

# an old install replaced whole (what it holds is the release's); anything else there is left alone
if [ -e "$dir" ]; then
	[ -x "$dir/bin/yelc" ] || fail "$dir is there and is no yel install: move it, or set YEL_INSTALL"
	rm -rf "${dir:?}"
fi
mkdir -p "$(dirname "$dir")"
mv "$work/yel-$target" "$dir"
echo "yel: $("$dir/bin/yelc" --version) in $dir"

case ":$PATH:" in
*":$dir/bin:"*) ;;
*)
	echo
	echo "Add it to your PATH (in ~/.zshrc, ~/.bashrc or ~/.profile):"
	echo
	echo "  export PATH=\"$dir/bin:\$PATH\""
	;;
esac
