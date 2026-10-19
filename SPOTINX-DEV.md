# SpotinX direct-launch experimental build

This branch is an **unverified proof of concept** based on Chromium NX.
The existing Nintendo Switch Kiosk configuration works, but this customized
binary has **not yet been compiled or tested**.

## Objective

Build a separate `SpotinX.nro` that directly runs Chromium (No GPU)
at `https://open.spotify.com/` in kiosk mode, without reading the Chromium NX
menu. It uses the current working SD files:
- `sdmc:/switch/chromium-nx/chromium-full.ext4`
- `sdmc:/switch/chromium-nx/profile-switchify-kiosk.ext4`

These files stay in place; do not move or delete them. SpotinX is a
separate NRO, but it shares the Chromium NX asset folder and its log path.
**Do not run SpotinX and Chromium NX simultaneously.**

## Build differences

`SPOTINX=1 bash gx/build-nx.sh` selects a separate build variant:
`-DSPOTINX` in gx/host_nx.c, metadata `SpotinX`, and the
`gx/dist-nx/switch/chromium-nx/SpotinX.nro` output.

Important: `gx/build-nx.sh` still requires the original Chromium NX
build environment: devkitA64, matching LKL source and patches, and PC-built
Chromium images/sysroot from `build-pc.sh`/`build-chromium.sh`.
Simply running it on macOS will not work. GitHub Actions integration remains
to be implemented and validated; this branch has **no working CI build yet**.

## Switch setup after a successful build

Copy **only** the compiled `SpotinX.nro` to a separate homebrew path,
e.g. `sdmc:/switch/SpotinX/SpotinX.nro`. Keep existing
`/switch/chromium-nx/` and `programs.txt` unchanged. Continue using
full-memory/39-bit address-space launch, as required by Chromium NX.
The original Chromium NX NRO remains the fallback.

## Current limitations

Spotify authentication and music playback are **not verified**. Chromium
NX may not provide Widevine DRM; Spotify web playback may remain unsupported
even if the UI loads. No Nintendo system browser is used. The application
is unofficial and not affiliated with Spotify.
