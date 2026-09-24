# Git workflow

Repository: `https://github.com/mbahmani90/cylinko_firmware` — default branch `master`.
Every change goes through a short-lived branch and a pull request; nothing is
committed directly on `master`.

## "upload in github"

When the maintainer says **"upload in github"** it means run the whole
sequence below, end to end, for the current working-tree changes:

| # | Step | Command |
|---|---|---|
| 1 | Create a branch from an up-to-date `master` | `git checkout master && git pull` · `git checkout -b <type>/<short-description>` |
| 2 | Commit the changes | `git add <files>` · `git commit` |
| 3 | Push the branch | `git push -u origin <branch>` |
| 4 | Create the pull request | `gh pr create --base master --title "<title>"` |
| 5 | Add a description to the PR | `--body` / `--body-file` on the same `gh pr create` call |
| 6 | Merge the PR | `gh pr merge <n> --merge --delete-branch` |
| 7 | Switch to `master` | `git checkout master` |
| 8 | Pull | `git pull` |

Result: `master` is up to date locally and remotely, the feature branch is
deleted on GitHub, and the working tree is clean.

Step 1 always starts from `master`, never from another feature branch: switch
to `master` and pull first so the new branch is based on the latest merged
commit and the PR contains only its own change. (If the working tree already
holds uncommitted changes, `git checkout master` carries them along as long as
they don't conflict; otherwise `git stash` before and `git stash pop` after
creating the branch.)

### Conventions

**Branch names** — `<type>/<kebab-case-description>`, where `<type>` is one of:

| Type | Use for |
|---|---|
| `fix/` | bug fixes |
| `feat/` | new functionality |
| `refactor/` | behaviour-preserving restructuring |
| `docs/` | documentation only |
| `chore/` | build, tooling, housekeeping |

Examples from history: `fix/tcp-recv-timeout-so-rcvtimeo`,
`docs/tcp-server-and-drop-ip4-filter`, `refactor/cy-log-macros`.

**Commit message** — first line `<Module>: <what changed>` (≤ 72 chars), a blank
line, then *why* and any behavioural side effects in the body. Example:

```
TcpServer: replace per-message timeout task with SO_RCVTIMEO

The old timeout task raced with the receive loop ...
```

**PR title** — same as the commit's first line (one commit per PR is the norm).

**PR description** — three sections:

```
## Summary
- what changed, one bullet per logical change

## Behavioural side effects   (omit if none)
- anything a reviewer or tester must know

## Test plan
- [x] what was verified and how
- [ ] what still needs hardware (idf.py build + flash, nc test, ...)
```

**Merge** — regular merge commit (`--merge`, not squash/rebase) so the branch
history stays visible, and delete the remote branch.

### Full example

```bash
git checkout master
git pull
git checkout -b fix/udp-timeout
git add src/UdpServer.c
git commit -m "UdpServer: ..."
git push -u origin fix/udp-timeout
gh pr create --base master --title "UdpServer: ..." --body-file pr.md
gh pr merge --merge --delete-branch
git checkout master
git pull
```

## Release — tag every stable state

The repo is also the **CyThing library** (see [cy_thing_lib.md](cy_thing_lib.md)),
and both library registries and the device itself carry a version number.
Whenever `master` is in a **stable condition** — the hardware checklist for
the merged work has passed and there is nothing half-finished on `master` —
cut a release so that state has a name that users, the app and OTA can refer
to. Don't let stable states go by untagged: a version that only exists as a
commit hash cannot be pinned by `lib_deps`, offered by Library Manager, or
recognised in a `GET_INFO` reply.

### "release on github"

When the maintainer says **"release on github"** it means cut a release from
the current `master`, end to end:

| # | Step | Command |
|---|---|---|
| 1 | Be on an up-to-date, clean `master` | `git checkout master && git pull` |
| 2 | Pick the version | given in the phrase (`release on github 1.2.0`) → that; `… minor` / `… major` → bump that part of the latest tag; **bare phrase → PATCH bump** of the latest tag (`1.0.3` → `1.0.4`); no tag yet → `1.0.0` |
| 3 | Bump the three version fields, commit, tag, push | `scripts/release.sh <version>` |
| 4 | GitHub Release page with generated notes + source ZIP | `gh release create <version> --generate-notes` |
| 5 | Report | version, commit, tag URL, and the `lib_deps = …git#<version>` line |

The phrase is the authorisation for all five steps — no step-by-step
confirmation. The script's own guards (clean tree, on `master`, in sync with
`origin`, tag not taken) still apply, so a slip cannot produce a broken tag.

### What the script does

```bash
scripts/release.sh 1.1.0
```

It puts the one number into the three places that carry it, commits, tags and
pushes:

| File | Field | Read by |
|---|---|---|
| `library.properties` | `version=` | Arduino Library Manager (per git tag) |
| `library.json` | `"version"` | PlatformIO (`pio pkg`, `lib_deps … #1.1.0`) |
| `src/device_config/device_config.h` | `FIRMWARE_VERSION` | the device — `GET_INFO` and MQTT get-info replies |

`partitions/cything-4MB.csv` is duplicated in the phone app
(`FirmwareSketchBundle.kt`, `PARTITIONS_4MB_CSV`), which ships it in every
generated sketch. Change that table and update the app copy in the same breath,
or downloaded sketches flash against the wrong layout. The same goes for the
symbol list in `main/cything_config.cpp.example` /
`examples/Basic/cything_config.ino` and the app's generated `cything_config.ino`.

then `git commit -m "Release 1.1.0"`, `git tag -a 1.1.0`, `git push origin
master 1.1.0`. It refuses to run off `master`, on a dirty tree, when local and
`origin/master` differ, or when the tag already exists — so the tag always
points at exactly what was merged and tested.

Rules:

- **Semantic versions**, `MAJOR.MINOR.PATCH`, no `v` prefix (Arduino's indexer
  wants the bare number in `library.properties`; the tag name matches it).
  Bump MAJOR when a sketch would have to change (hook signatures, partition
  table, reply formats), MINOR for new commands/features, PATCH for fixes.
- **Tag only from merged `master`** — never from a feature branch. The
  "Release x.y.z" commit is the one thing that lands on `master` without a
  PR: it is mechanical (three version strings) and the script is the review.
- **A tag is immutable.** A mistake gets a new PATCH release, never a moved
  or deleted tag — registries and devices may already have seen it.
- `gh release create 1.1.0 --generate-notes` (step 4 above) adds a GitHub
  Release page with a ZIP — handy for Arduino IDE users while the repo is
  private; the git tag alone is what the registries need.

## Notes

- The repo has no CI, so there are no checks to wait for before merging; the
  test plan in the PR description is the record of what was verified — and
  the PR's hardware checklist passing is what makes the next release-worthy
  "stable condition".
- `build/` and `.vscode/` are ignored — never commit them.
- Local tooling: the ESP-IDF toolchain lives under `~/.espressif/`; `idf.py` is
  not on `PATH` by default (`source ~/.espressif/activate_idf_v6.0.2.sh`).
