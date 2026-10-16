#!/bin/sh
# Puts back the amdgpu X driver from before install.sh (for when X doesn't
# start with the patched one) and removes the patch so a rebuild stays
# unpatched. Run from the text console: doas sh rollback.sh, then log in.
# colormode can stay: without the driver's HDR_OUTPUT_METADATA it only does
# its SDR colour work.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with: doas sh $0" >&2; exit 1; }
keep=/var/lib/x11-amdgpu-hdr
[ -f "$keep/amdgpu_drv.so" ] || { echo "no saved driver in $keep" >&2; exit 1; }
cp "$keep/amdgpu_drv.so" /usr/lib64/xorg/modules/drivers/amdgpu_drv.so
rm -f /etc/portage/patches/x11-drivers/xf86-video-amdgpu/hdr-output-metadata.patch
echo "old driver back; log in again (later: emerge -1 xf86-video-amdgpu to match Portage)"
