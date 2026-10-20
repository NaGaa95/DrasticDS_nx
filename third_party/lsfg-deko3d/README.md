# Native LSFG library

Copied from `third_party/lsfg-deko3d` in the user-supplied NetherSX2_nx checkout,
originally adapted from Dolphin-NX's `Externals/lsfg`. This is the
portable DLL extraction, SPIR-V translation, UAM compilation, shader cache and
compute-graph planning library. DraStic's native GPU execution is in
`source/lsfg/drastic_deko_lsfg.cpp`.

Original file notices are preserved. Source files marked GPL-3.0-or-later use the
license in `LICENSE.md`; the adapted build integration and UAM bridge retain their
GPL-2.0-or-later notices. No Lossless Scaling DLL is included.
