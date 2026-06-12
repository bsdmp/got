# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Got (Game of Trees) is a version control system that uses Git repositories as
its on-disk storage format. It is developed exclusively on OpenBSD, targets
OpenBSD developers, is ISC-licensed, and is designed around `pledge(2)` and
`unveil(2)` sandboxing. Got can interoperate with Git on the same repository;
anything not yet implemented in Got can be done with `git`.

This is BSD source, not GNU/Linux userland: it builds with BSD `make` (using
`bsd.subdir.mk`/`bsd.prog.mk`/`bsd.lib.mk` from an OpenBSD source tree, i.e.
`/usr/share/mk`), and is written in C in OpenBSD kernel/userland style
(`style(9)`). Expect this repo to only fully build and test on OpenBSD.

## Build commands

```
$ make obj              # required first, out-of-tree object dirs
$ make                  # builds got, tog, and libexec/* helpers
$ make install          # installs to ~/bin (dev build) or $PREFIX (release)
```

Additional component groups, each with matching `-install` targets:

```
$ make server && make server-install   # gotd, gotctl, gotsh, gitwrapper
$ make webd   && make webd-install     # gotwebd, gotwebctl (FastCGI web UI)
$ make sysd   && make sysd-install     # gotsysd, gotsysctl
```

A release build (installs under `/usr/local` instead of `~/bin`) is produced
with `GOT_RELEASE=Yes`, e.g. `make GOT_RELEASE=Yes`. Version info lives in
`got-version.mk`.

Profiling a single helper (profiled builds cannot use `pledge(2)`):

```
$ cd libexec/got-read-pack
$ make clean && make PROFILE=1 && make install
```

## Test commands

Tests require binaries to be installed first (`make install`), and shell-based
tests additionally depend on `git(1)`. Tests using `got clone`/`fetch`/`send`
need `ssh 127.0.0.1` to succeed non-interactively. HTTP protocol tests need
the Perl `HTTP::Daemon` module (`pkg_add p5-http-daemon` on OpenBSD).

```
$ make regress                                    # full client-side suite
$ make regress GOT_TEST_PACK=1                     # against packed repos
$ make regress GOT_TEST_PACK=ref-delta              # ref-delta packed repos
$ make regress GOT_TEST_ALGO=sha256                 # sha256 object IDs
$ make regress GOT_TEST_ROOT=~/got-test             # avoid /tmp (unveiled)
```

`GOT_TEST_PACK` and `GOT_TEST_ALGO` can be combined.

Run a single cmdline test (targets are defined in `regress/cmdline/Makefile`,
one per `got` subcommand plus a few extras like `pack`, `dump`, `memleak`):

```
$ cd regress/cmdline
$ make checkout                    # or: ./checkout.sh -q -r "$GOT_TEST_ROOT"
$ ./log.sh -r ~/got-test           # run a single script directly
```

`tog`'s test suite lives separately and also runs via `make regress`:

```
$ cd regress/tog
$ make            # all tog tests
$ ./log.sh        # a single tog test
```

Server-side suites (each requires the matching component built/installed,
and gotd/gotwebd tests must run as root — see `regress/gotd/README` and
`regress/gotwebd`):

```
$ make server-regress     # gotd, via regress/gotd
$ doas make webd-regress  # gotwebd, via regress/gotwebd
```

`gotsysd` has its own suite under `regress/gotsysd` (see `regress/gotsysd/README`).

Some regression tests are plain C unit tests (see e.g. `regress/idset`,
`regress/path`, `regress/delta`, `regress/deltify`) rather than shell scripts;
prefer these only when a bug cannot be triggered through the CLI.

`util/got-build-regress.sh` is the script used for the project's own
build/test CI loop (clean build, full regress matrix across pack/sha256
combinations, optional gotd/gotwebd tests, then a release-mode build) — useful
as a reference for "what does a full verification pass look like."

## Architecture

### Component map

- `got/` — the `got` CLI frontend (`got.c`), dispatches to `lib/` functions.
- `tog/` — ncurses-based interactive repository browser.
- `lib/` — the core library implementing Git object/pack/worktree logic,
  shared by `got`, `tog`, `gotadmin`, and the daemons. Public API headers are
  in `include/got_*.h`; internal-only headers used within `lib/` are
  `lib/got_lib_*.h`.
- `libexec/got-*` — small privilege-separated helper programs (see below).
- `gotd/` — the repository server daemon (protocol read/write access over
  SSH/HTTP).
- `gotctl/` — control utility for `gotd`.
- `gotsh/` — restricted login shell for users accessing `gotd` over SSH.
- `gitwrapper/` — like `mailwrapper(8)`, dispatches `git-upload-pack`/
  `git-receive-pack` invocations appropriately (Got vs Git).
- `gotwebd/` + `gotwebctl/` — FastCGI web UI for browsing repositories,
  designed to run under `httpd(8)`; `template/` provides the `.tmpl` templating
  engine (`ports`-style, parsed by `template/parse.y`) it uses for HTML output.
- `gotsysd/` + `gotsysctl/` + `gotsys/` — daemon/tooling to manage a `gotd`
  server by committing configuration to a special `gotsys.git` repository;
  `gotsys/` holds the config file grammar/format.
- `gotadmin/` — low-level repository administration commands.
- `cvg/` — CVS-to-Got-related tooling (only built for non-release checkouts).
- `regress/` — all test suites, one subdirectory per area.
- `util/` — maintainer scripts (e.g. the build/regress automation script).

Man pages live alongside their programs (e.g. `got/got.1`,
`got/got-worktree.5`, `gotd/gotd.8`) and are a good source of authoritative
behavioral documentation — check them before assuming CLI/config behavior.

### Privilege separation (privsep)

This is the central architectural pattern of the codebase, documented in
`lib/got_lib_privsep.h`. All code runs as the same UID, but sensitive/complex
parsing logic (decompression, object/pack parsing, gitconfig/gotconfig
parsing, network fetch/send) is executed in separate child processes spawned
via `fork(2)`+`exec(2)` of the small helper binaries in `libexec/` (e.g.
`got-read-object`, `got-read-pack`, `got-fetch-pack`, `got-index-pack`,
`got-read-gitconfig`). Each helper runs under a tight `pledge(2)` promise set
(commonly just `"stdio recvfd"`).

Communication between the main process and these helpers happens via
`imsgbuf_flush(3)`/`imsgbuf_read(3)` imsg buffers (OpenBSD's `imsg(3)` IPC),
with file descriptors passed across the boundary for bulk data (pack files,
large blobs) rather than copying bytes through imsg messages. Message types
are enumerated in `got_imsg_type` (`lib/got_lib_privsep.h`); one enum covers
object reads, fetch/send networking, pack indexing, and more. This same imsg
pattern is reused by `gotd` (see `lib/gotd_imsg.c`) and `gotsysd` (see
`lib/gotsys_imsg.c`) for their own privilege separation between daemon and
per-connection/session child processes.

When touching any code that reads repository data or talks to a remote, check
whether the logic belongs in `lib/` (orchestration, runs in the
less-privileged main process) or in the corresponding `libexec/got-*` helper
(the actual parsing/decompression, runs sandboxed) — new imsg message types
usually need a matching pair of send/receive functions in `lib/privsep.c` and
handling code in the helper's `main()`.

### Work tree model

`got_worktree_*` functions in `lib/worktree.c` (and `lib/worktree_cvg.c` for
the CVS-adjacent `cvg` tool) implement the on-disk checkout format, tracked
under a `.got/` directory in the work tree root (analogous to git's `.git/`
for a checkout) — see `got/got-worktree.5` for the on-disk format spec, and
`got/git-repository.5` for how Got interprets a bare Git repository. File
indexing/status tracking is in `lib/fileindex.c`.

### got.c command dispatch

`got/got.c` defines commands as entries in a `static const struct
got_cmd got_commands[]` table (name, function pointer, usage string); adding
a new `got` subcommand means adding a `cmd_foo`-style handler function and a
row in that table, plus a corresponding section in `got/got.1`.

### Diff engine

`lib/diff_*.c` implements a full diff subsystem (Myers and patience
algorithms in `diff_myers.c`/`diff_patience.c`, atomize/tokenize in
`diff_atomize_text.c`, output formatters for unified diff and ed script in
`diff_output_*.c`) used by `got diff`, `got blame` (`lib/blame.c`), and
`tog`'s diff view — this is a fairly self-contained subsystem if you need to
change diff behavior.

## Contribution norms (from README)

- Got is developed via mailing list, not GitHub PRs — "Pull requests via any
  Git hosting sites will likely be overlooked." Patches/bug reports go to
  `gameoftrees@openbsd.org`.
- Bug reports are expected to include a reproduction recipe written as a
  shell script under `regress/cmdline/`, ideally as an "xfail" (expected
  failure) regression test that later gets flipped to passing once fixed.
- A regression test is expected for any behavioral fix, both to demonstrate
  the bug and to prevent regressions.
