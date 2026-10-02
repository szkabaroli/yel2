#!/bin/sh
# libuv compiled into $1/libuv.a (once: an archive already there is kept), for this system
set -e
out=${1:-build}
[ -f "$out/libuv.a" ] && exit 0
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out/libuv-objects"
common="fs-poll idna inet random strscpy strtok thread-common threadpool timer uv-common uv-data-getter-setters version
unix/async unix/core unix/dl unix/fs unix/getaddrinfo unix/getnameinfo unix/loop-watcher unix/loop unix/pipe unix/poll
unix/process unix/random-devurandom unix/signal unix/stream unix/tcp unix/thread unix/tty unix/udp unix/proctitle"
case "$(uname -s)" in
Darwin)
	system="unix/bsd-ifaddrs unix/kqueue unix/random-getentropy unix/darwin-proctitle unix/darwin unix/fsevents"
	defines="-D_DARWIN_UNLIMITED_SELECT=1 -D_DARWIN_USE_64_BIT_INODE=1" ;;
Linux)
	system="unix/linux unix/procfs-exepath unix/random-getrandom unix/random-sysctl-linux"
	defines="-D_GNU_SOURCE -D_POSIX_C_SOURCE=200112" ;;
*) echo "libuv: $(uname -s) is not built yet" >&2; exit 1 ;;
esac
objects=""
for f in $common $system; do
	o="$out/libuv-objects/$(echo "$f" | tr / -).o"
	${CC:-cc} -O2 -w -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE $defines -I"$here/include" -I"$here/src" -c "$here/src/$f.c" -o "$o"
	objects="$objects $o"
done
ar rcs "$out/libuv.a" $objects
