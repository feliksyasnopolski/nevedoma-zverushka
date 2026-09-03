# Graph input configuration

The current carve is represented by the canonical source layers under
`vendor/init-corpus/`. There is no separate executable include list: the
compiler parses `vendor/init-corpus/mutable/`, and the mutable tree is the
maintained selection result.

The immutable layer is retained for source comparison and provenance. It is
not merged automatically by the current compiler. Compiler-side transformations
such as early-module suppression, native-owned command exclusions, and the
measured module-readiness barrier are documented in `docs/INIT_CORPUS.md` and
remain compiler behavior until that implementation is promoted separately.
