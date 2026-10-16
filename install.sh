#!/bin/sh
# Installs HDR for X11 on amdgpu (needs root), on Gentoo:
#   - the xf86-video-amdgpu patch into /etc/portage/patches, then rebuilds the
#     driver from source (Portage would otherwise reuse a binary package);
#     the driver from before is kept in /var/lib/x11-amdgpu-hdr for rollback.sh
#   - colormode into /usr/local/bin, the colormode-hdr mpv plugin into
#     /usr/local/lib
# Then add config/xinitrc to ~/.xinitrc, config/mpv.conf to mpv.conf, and log
# in again (the new driver loads with X).
# Run: doas sh install.sh   (or sudo)
set -eu
[ "$(id -u)" = 0 ] || { echo "run with: doas sh $0" >&2; exit 1; }
cd "$(dirname "$(readlink -f "$0")")"
drv=/usr/lib64/xorg/modules/drivers/amdgpu_drv.so
keep=/var/lib/x11-amdgpu-hdr
[ -f "$drv" ] || { echo "no amdgpu X driver at $drv" >&2; exit 1; }
command -v emerge >/dev/null || { echo "not Gentoo: build xf86-video-amdgpu with the patch yourself (README)" >&2; exit 1; }
mkdir -p "$keep"
if ! strings "$drv" | grep -q HDR_OUTPUT_METADATA; then
	cp -p "$drv" "$keep/amdgpu_drv.so"
	command -v quickpkg >/dev/null && quickpkg x11-drivers/xf86-video-amdgpu >/dev/null || true
fi
mkdir -p /etc/portage/patches/x11-drivers/xf86-video-amdgpu
cp portage/patches/x11-drivers/xf86-video-amdgpu/hdr-output-metadata.patch \
	/etc/portage/patches/x11-drivers/xf86-video-amdgpu/
emerge --oneshot --usepkg=n --getbinpkg=n x11-drivers/xf86-video-amdgpu
strings "$drv" | grep -q HDR_OUTPUT_METADATA || { echo "the rebuilt driver has no HDR_OUTPUT_METADATA: patch not applied?" >&2; exit 1; }
cc -O2 -o /usr/local/bin/colormode bin/colormode.c -lX11 -lXrandr -lm
mkdir -p /usr/local/lib
cc -O2 -shared -fPIC -o /usr/local/lib/colormode-hdr.so bin/colormode-hdr.c -lX11
echo "installed. Add config/xinitrc and config/mpv.conf, then log in again."
echo "If X doesn't start after that: doas sh $(pwd)/rollback.sh from the text console."
