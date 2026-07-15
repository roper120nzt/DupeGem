# Vendored BLAKE3

These files are the official BLAKE3 C implementation from
`BLAKE3-team/BLAKE3`, pinned at commit
`8aa5145039b972ba30e98e788752d37d14568824`.

DupeGem builds the portable implementation plus the Windows GNU x86-64 SIMD
assembly variants. Runtime CPU dispatch selects SSE2, SSE4.1, AVX2, or AVX-512
when available. The upstream Apache 2.0 and CC0 licenses are included.
