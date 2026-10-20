# Bundled gameplay artwork

The user-supplied Nintendo DS hardware bezel from PNGkey and eight lnkd skin
backgrounds are documented in `pngkey-ds/NOTICE.md` and `lnkd/NOTICE.md`.
Their notices are separate from the MIT grant below.

## Adaptive PocketArch bezels

The White, Coral pink, Enamel navy and Black presets adapt the NDS bezel
palette, textured shell, edge shadow and black screen surround from
[PocketArch](https://github.com/stunrelay/PocketArch/tree/33dc23be7ea5938956bbe686c1c9494ef5628e30/Shaders/Bezel/NDS),
commit `33dc23be7ea5938956bbe686c1c9494ef5628e30`.
Copyright (c) 2025 stunrelay, MIT license (`pocketarch/LICENSE`).
The original white shader is retained in `pocketarch/` as a reference.

DrasticDS_nx renders the adapted design once on the CPU when the layout
changes, masks all active game rectangles, and caches it in the existing
overlay. It does not execute the original RetroArch shader or remap touch
coordinates. Unlike the original fixed dual-screen design, the adaptation
works with single-screen, hybrid and custom layouts as well.
