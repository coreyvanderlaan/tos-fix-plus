# tos-fix-plus: notes for Claude sessions

The public release of TOSFIXPLUS. Development, diagnostics and test tooling live in the private
`E:\Projects\tos-60fps` repo (capture, burst, replay test, trace, install and drive scripts);
this repo is only what ships.

- Keep it clean: F9 is the only in-game feature and `MaxFPS` the only setting. New diagnostics
  go in tos-60fps, not here.
- Behaviour changes are made and tested in tos-60fps first, then ported here; the logic in
  `src/` should stay equivalent to tos-60fps's `src/`.
- Documentation is for players and contributors, not for Corey: README.md (install, use,
  build), docs/HOW-IT-WORKS.md (design).
- Build with `build.bat`. Publishing (making the repo public, releases) needs Corey's go-ahead.
