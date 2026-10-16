# x11-amdgpu-hdr

HDR video on X11 with an AMD GPU, no Wayland and no compositor: a small patch that lets the amdgpu X driver send HDR metadata to the panel, and `colormode`, which puts the screen into HDR mode while an HDR video is fullscreen and on screen and back to normal SDR the moment you switch away. mpv (through a small plugin) and [media-player](https://github.com/r3clusionn/media-player) render real HDR (PQ) while it is on. A switch (`colormode auto on|off`) turns the whole thing on or off.

**Status:** in daily use on an HP OmniBook 7 14-kh0xxx (Ryzen AI 9 HX 475, Radeon 890M, Samsung 2880x1800 120 Hz OLED, 1107 nits peak) with Gentoo, Linux 7.2, Xorg 21.1, xf86-video-amdgpu 25.0, Mesa 26.1, mpv 0.41 and dwm. Tested with a test card and an HDR10 clip in mpv and media-player.

![Terminal output on a dark background, three commands. "colormode status" prints "eDP: srgb", the panel's primaries "R 0.683,0.316  G 0.245,0.714  B 0.140,0.044  white 0.3135,0.3291" and "HDR: PQ, peak 1107 nits, full screen 497, black 0.0002; SDR white 347; metadata none; switch on". "colormode auto" prints "HDR for fullscreen HDR video: on". "tail -4" of colormode's log shows four switches a few seconds apart: "eDP: HDR", "eDP: SDR", "eDP: HDR", "eDP: SDR".](docs/images/terminal.png)

## Features

- **Real HDR under X.** The panel gets HDR10 metadata (PQ, BT.2020) and the video arrives as PQ in 10 bits, so highlights go up to the panel's peak and colours use its full wide gamut.
- **Only when it should.** HDR is on while a window that shows HDR video is fullscreen, mapped and covering the screen. Leave fullscreen, or switch to another tag (dwm moves hidden tags' windows off screen), and the screen is SDR again, with the desktop exactly as before. Several quick switches in a row settle on the last one.
- **A switch.** `colormode auto off` keeps the screen in SDR for good (HDR video is then tone mapped, as before); `on` brings it back. The setting stays across logins, and a running `colormode watch` follows it at once.
- **Players.** mpv with the `colormode-hdr` plugin, and media-player built in: while the screen is HDR they render PQ tone mapped to the panel's peak, with subtitles, OSD and media-player's controls at the desktop's white, not glaring. Afterwards their own settings come back.
- **No cost when idle.** The conversion is done by the display hardware; `colormode watch` sleeps until a window changes.
- **A test card** (`colormode test`) that switches SDR, HDR, SDR, HDR by itself and goes back to SDR, with a separate watchdog process in case it hangs.

## How to install

Requires Xorg with the amdgpu driver (xf86-video-amdgpu 25.0), a panel whose EDID lists the PQ curve, Linux with amdgpu's display core (any recent kernel), a C compiler, libX11 and libXrandr. mpv with Vulkan and C plugins (`cplugins`, on by default) for the mpv part.

On Gentoo:

```sh
git clone https://github.com/r3clusionn/x11-amdgpu-hdr
cd x11-amdgpu-hdr
doas sh install.sh            # (or sudo) patch, rebuild the driver, colormode, plugin
```

`install.sh` copies the patch to `/etc/portage/patches/x11-drivers/xf86-video-amdgpu/`, keeps the current driver in `/var/lib/x11-amdgpu-hdr`, rebuilds the driver from source (`--usepkg=n`: with `--usepkg` in `EMERGE_DEFAULT_OPTS` Portage would silently reinstall a binary package without the patch) and compiles `colormode` and the plugin. The rebuild needs no new packages beyond its build helper (dev-build/meson-format-array, removable with `emerge --depclean` afterwards).

Then:

- **At login:** add the lines in `config/xinitrc` to `~/.xinitrc`, before the window manager.
- **mpv:** add the lines in `config/mpv.conf` to `~/.config/mpv/mpv.conf`.
- Log out and back in: the patched driver loads with X. If X doesn't start, run `doas sh rollback.sh` from the text console and log in again.

Elsewhere: apply `portage/patches/x11-drivers/xf86-video-amdgpu/hdr-output-metadata.patch` to xf86-video-amdgpu 25.0 (`patch -p1`), build it with meson as usual, install `amdgpu_drv.so`, and compile the two programs with the `cc` lines in `install.sh`.

## How to use

```sh
colormode auto on        # HDR for fullscreen HDR video (the default)
colormode auto off       # always SDR; HDR video tone mapped as before
colormode auto           # what is set
colormode status         # mode, the panel's HDR range, metadata, the switch
colormode test           # 20 s SDR/HDR test card, back to SDR by itself
colormode srgb           # put the desktop back if anything looks wrong
```

With the switch on, play an HDR file (HDR10, HLG or Dolby Vision) in mpv or media-player and make it fullscreen. The screen blanks for a moment and is in HDR; switch tags or leave fullscreen and it blanks again and is SDR. `tail -f $XDG_RUNTIME_DIR/colormode.log` shows each switch.

The test card's bar shows the phase (left SDR, middle HDR with BT.2020 colours, right HDR with the panel's own colours). Greys and colours should look the same in every phase; the row of whites is one white in SDR and steps up in HDR.

## How it works

- **The missing piece in X.** The kernel's amdgpu driver has had `HDR_OUTPUT_METADATA` (the HDR infoframe, sent to an eDP panel as a secondary data packet) for years, and any DRM master can set it, including the X server through the old property call. The amdgpu X driver just never exposed it. The patch (about 90 lines in `drmmode_display.c`) publishes it as a RandR output property: a byte array holding the kernel's 32-byte `struct hdr_output_metadata`, or a single 0 byte for none. It is kept outside the driver's property list, because the driver finds its TearFree pseudo-property by its place at the end of that list.
- **What this panel needs**, found with the test card: PQ metadata alone did nothing (the panel showed the PQ signal as SDR, washed out); it enters HDR only with BT.2020 colorimetry signalled too (the connector's `Colorspace` set to `BT2020_RGB`). SDR metadata (EOTF 0) made it crush dark greys, and removing the metadata alone didn't take it out of HDR: only a full modeset does. So `colormode` uses two states: HDR is `Colorspace BT2020_RGB` plus PQ metadata, SDR is `Default` with no metadata at all. Changing `Colorspace` is a full modeset, hence the short blank each way.
- **The metadata** describes the panel itself (its EDID primaries, 1107 nits peak, 497 nits full-screen average): the players tone map to the panel, so the panel has nothing left to map.
- **The colour tables.** For SDR, `colormode srgb` converts sRGB to the panel's gamut in the display engine (`DEGAMMA_LUT`, `CTM`, `GAMMA_LUT`). For HDR it keeps the two sRGB curves, whose round trip is exact (every 10-bit code comes back within 0.1), and sets an identity matrix, so the PQ picture reaches the panel unchanged.
- **The protocol.** A player showing HDR video sets `_COLORMODE_HDR_VIDEO` (CARDINAL 1) on its top-level window and sends the root window a client message of that name. `colormode watch` tracks such windows (`_NET_WM_STATE_FULLSCREEN`, map state, geometry) and, while one covers an HDR output, switches it and sets `_COLORMODE_HDR_OUTPUT` on the window: on, SDR white in nits, the panel's peak in nits, primaries (0 BT.2020). Players are told before the screen switches in and after it switches out, so a switch shows a washed-out frame rather than glare. The SDR white is the panel's brightness: amdgpu sets this OLED's light level in millinits over the panel's AUX channel, so the backlight value is the white the desktop shows.
- **mpv** renders PQ on X when told to: `target-trc=pq`, `target-prim=bt.2020`, `target-peak`, `hdr-reference-white` (subtitles and OSD) and `target-colorspace-hint=no`, because Mesa's X11 Vulkan path only offers an sRGB swapchain colour space (`A2R10G10B10` + `SRGB_NONLINEAR` at depth 30) and the bits pass through it unchanged. The plugin sets these only while the screen is HDR and restores mpv's own values afterwards.
- **media-player** does the same for its embedded mpv, and converts its page and popups (CEF, sRGB) to PQ at the SDR white in its compositor's shader while the screen is HDR.

## Measurements

On the OmniBook 7, at depth 30.

- **Switch time:** a short blank each way (a full eDP modeset, about 1–2 s).
- **media-player's controls in HDR:** a white UI pixel is drawn as 0.63, which is PQ for the 347-nit desktop white; unconverted it would be 1.0 (10,000 nits, clipped to the panel's peak).
- **Near black through the hardware curves** (simulated from the kernel's code: a 4096-entry LUT over 13 segments from 2^-12): converting the desktop to PQ in the colour tables would shift only the two darkest 10-bit levels. Kept here for anyone trying an always-HDR desktop.
- **`colormode watch`:** one small process, asleep between window changes.

## Verification

- **Test card:** with BT.2020 colorimetry, the HDR phases show the greys and colours of the SDR phases, the row of whites steps up, and the near-black steps stay visible; without it, the HDR phases came out washed out with no step-up.
- **HDR10 clip** (Jellyfin's 1080p HEVC HDR10 test video): in mpv, fullscreen shows deeper blues and purples than the tone-mapped window; Alt+F and Alt+number switch with one blank each, and Thorium and Discord look normal afterwards.
- **media-player:** the clip played fullscreen through its player switched the screen to HDR; its mpv reported `target-trc=pq`, `target-prim=bt.2020`, `target-peak=1107`, `hdr-reference-white=347`; its controls looked normal.
- **Logic without a screen:** an invisible stand-in window (input-only, empty input shape) went through ten states (on screen, off screen, back, four quick switches, fullscreen off and on, an SDR file, unmapped, mapped) and `colormode watch` decided each correctly. The mpv plugin was checked in a real mpv in a hidden window.
- **SDR unchanged:** with the patched driver and the switch off, `colormode srgb` sets byte-identical colour tables to before.

## Limits

- Each switch blanks the screen for a moment: this panel only leaves HDR after a full modeset.
- Fullscreen only. There is no HDR in a window: X has no compositor to mix HDR and SDR windows, and the kernel won't give the X server the atomic API that per-plane colour pipelines need (it refuses any process whose name starts with "X").
- Only players that speak the protocol: mpv with the plugin, and media-player. Browsers on X11 (Chromium, Thorium) never output HDR; Vulkan games can't ask for it (no HDR colour spaces in Mesa's X11 path).
- The brightness keys work as usual in SDR; whether they change an HDR picture (PQ is absolute light) hasn't been checked.
- Written for one panel. Other panels may enter HDR without BT.2020 colorimetry, or leave it without a modeset; `colormode test` shows what yours does.
- Gentoo install script; elsewhere the driver has to be patched and built by hand.

## License

MIT (see `LICENSE`).
