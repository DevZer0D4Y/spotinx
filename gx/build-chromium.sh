#!/bin/bash
# Makes the root filesystem Chromium runs from (an ext4 image, for gx to mount)
# out of Debian's arm64 chromium-headless-shell, and runs it under qemu-user.
#
#   docker exec lkl-gx bash /project/gx/build-chromium.sh [chromium arguments]
#
# Without arguments, Chromium prints the DOM of a test page once its script
# has run. With "shot [url]", it renders the page (the test page by default)
# in a 1280x720 window and the picture ends up in chromium.png. With
# "browse <script> [url]", it shows the page continuously, as on the Switch:
# the pictures go to chromium-frames/ and the script plays the user (see
# host_input() in host_posix.c). GX_TRACE=1 traces the guest's syscalls. Run
# build-pc.sh first: it builds gx.
set -u

SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/work/gx-out}
ROOT=$OUT/chromium-root
IMAGE=$OUT/chromium.ext4
QEMU=qemu-aarch64-static
ARM64_LIB=/usr/lib/aarch64-linux-gnu
ARM64_LD=$ARM64_LIB/ld-linux-aarch64.so.1
CHROMIUM=/usr/lib/chromium/chromium-headless-shell

[ -x $CHROMIUM ] || { echo "no arm64 chromium-headless-shell in this container"; exit 1; }

if [ ! -e "$IMAGE" ] || [ "$0" -nt "$IMAGE" ] || [ $CHROMIUM -nt "$IMAGE" ]; then
	echo "== root filesystem"
	rm -rf "$ROOT"
	# The image is mounted read-only: gx mounts memory filesystems on
	# these, and they have to be there.
	mkdir -p "$ROOT"/{proc,sys,dev,tmp,run,root,lib,etc/ssl/certs,usr/lib,usr/share/gx,usr/share/fonts}
	cp -r /usr/lib/chromium "$ROOT/usr/lib/" || exit 1

	# Every library the binary is linked against, and the ones NSS loads
	# by name at run time.
	# chromium-shell, if it is installed, is the same engine with a window
	# (the "wayland" mode below wants it).
	SHELL_BIN=/usr/lib/chromium/chromium-shell
	[ -x $SHELL_BIN ] || SHELL_BIN=
	for lib in $($QEMU $ARM64_LD --list $CHROMIUM | awk '$2 == "=>" { print $3 }') \
		$([ -z "$SHELL_BIN" ] || $QEMU $ARM64_LD --list $SHELL_BIN | awk '$2 == "=>" { print $3 }') \
		$ARM64_LIB/libsoftokn3.so $ARM64_LIB/libfreebl3.so $ARM64_LIB/libfreeblpriv3.so \
		$ARM64_LIB/libnssckbi.so $ARM64_LIB/libnssdbm3.so $ARM64_LIB/libsqlite3.so.0 \
		$ARM64_LIB/libnss_*.so.2 $ARM64_LIB/libgcc_s.so.1; do
		cp -L "$lib" "$ROOT/lib/" || exit 1
	done
	cp -L $ARM64_LD "$ROOT/lib/ld-linux-aarch64.so.1" || exit 1
	# For rendering with the GPU (GX_GPU=1 below): gx's EGL and GLES, which
	# are the host's (build-pc.sh makes them), and the small library a
	# Wayland client makes its GL window with.
	for lib in libEGL.so.1 libGLESv2.so.2; do
		[ ! -e "$OUT/sysroot/lib/$lib" ] || cp "$OUT/sysroot/lib/$lib" "$ROOT/lib/" || exit 1
	done
	[ ! -e $ARM64_LIB/libwayland-egl.so.1 ] || cp -L $ARM64_LIB/libwayland-egl.so.1 "$ROOT/lib/" || exit 1

	cp -rL /usr/share/fonts/truetype "$ROOT/usr/share/fonts/" || exit 1
	cp -rL /etc/fonts "$ROOT/etc/" || exit 1
	cp -L /etc/ssl/certs/ca-certificates.crt "$ROOT/etc/ssl/certs/" || exit 1
	echo "root:x:0:0:root:/root:/bin/sh" >"$ROOT/etc/passwd"
	echo "root:x:0:" >"$ROOT/etc/group"
	echo "127.0.0.1 localhost" >"$ROOT/etc/hosts"
	echo "hosts: files dns" >"$ROOT/etc/nsswitch.conf"
	echo "nameserver 10.0.2.3" >"$ROOT/etc/resolv.conf"
	echo "0123456789abcdef0123456789abcdef" >"$ROOT/etc/machine-id"
	# The key map gx's Wayland display hands out (wl.c), a standard one.
	if command -v xkbcli >/dev/null; then
		xkbcli compile-keymap --layout us >"$ROOT/usr/share/gx/keymap.xkb" || exit 1
	fi
	cat >"$ROOT/usr/share/gx/page.html" <<'EOF'
<!doctype html><html><head><title>gx page</title>
<style>body{background:#204080;color:#fff;font:40px sans-serif}
div{width:300px;height:120px;background:linear-gradient(90deg,red,lime);border-radius:20px}</style></head>
<body><h1 id=t>Bonjour depuis Chromium</h1><div></div><p id=js></p>
<script>document.getElementById("js").textContent = "JS: " + [1, 2, 3].map(x => x * x).join(",")</script>
</body></html>
EOF
	size=$(( $(du -sm "$ROOT" | cut -f1) * 13 / 10 + 32 ))
	rm -f "$IMAGE"
	mke2fs -q -t ext4 -O ^has_journal -d "$ROOT" "$IMAGE" "${size}M" || exit 1
	echo "$IMAGE: ${size} MB"
fi

echo "== chromium"
export_arg=
if [ "${1:-}" = wayland ]; then
	# The Chromium that has a window, on gx's Wayland display (wl.c). The
	# pictures go to chromium-frames/ and GX_INPUT is the user's script.
	shift
	rm -rf "$OUT/chromium-frames"
	mkdir -p "$OUT/chromium-frames"
	export GX_FRAMES=$OUT/chromium-frames GX_INPUT=${GX_INPUT:-}
	# GX_GPU=1: Chromium draws with GLES (through ANGLE's GLES backend and
	# gx's thunk libraries) instead of with the processor. An experiment.
	gpu="--disable-gpu"
	[ -z "${GX_GPU:-}" ] || gpu="--use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-sandbox --in-process-gpu ${GX_GPU_FLAGS:-}"
	exec timeout ${GX_TIMEOUT:-300} $QEMU "$OUT/gx" ${GX_TRACE:+-t} -n -w \
		-e XDG_RUNTIME_DIR=/run -e WAYLAND_DISPLAY=wayland-0 "$IMAGE" \
		/usr/lib/chromium/chromium-shell --no-sandbox --single-process --no-zygote \
		$gpu --js-flags=--jitless --ozone-platform=wayland \
		"${@:-file:///usr/share/gx/page.html}"
fi
if [ "${1:-}" = browse ]; then
	rm -rf "$OUT/chromium-frames"
	mkdir -p "$OUT/chromium-frames"
	export GX_FRAMES=$OUT/chromium-frames GX_INPUT=$2
	export_arg=-b
	set -- --remote-debugging-pipe --window-size=1280,720 --hide-scrollbars \
		"${3:-file:///usr/share/gx/page.html}"
elif [ $# -eq 0 ]; then
	set -- --dump-dom file:///usr/share/gx/page.html
elif [ "$1" = shot ]; then
	rm -f "$OUT/chromium.png"
	export_arg="-o /tmp/shot.png=$OUT/chromium.png"
	set -- --screenshot=/tmp/shot.png --window-size=1280,720 --hide-scrollbars \
		"${2:-file:///usr/share/gx/page.html}"
fi
# One process: gx has no fork. No GPU process either, for now. V8 without its
# compilers (--jitless): they need memory that is writable and executable at
# once, which Horizon does not have and gx refuses everywhere.
exec timeout ${GX_TIMEOUT:-600} $QEMU "$OUT/gx" ${GX_TRACE:+-t} -n $export_arg "$IMAGE" $CHROMIUM \
	--no-sandbox --single-process --no-zygote --disable-gpu --js-flags=--jitless "$@"
