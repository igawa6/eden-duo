# Eden Duo upstream-sync tools

Eden Duo tracks Eden nightly. These scripts keep that sync mechanical. They need
`bash`, `git` and `python3` (unlike the POSIX `sh` tools one level up) and must be
run from the repository root. Neither script ever pushes.

The upstream commit Duo is based on is recorded in `.upstream/eden.json`
(`source_commit`). The deliberate differences from upstream are listed in
`.upstream/DIVERGENCES.md`.

## `duo-touched-ledger.sh`

Read-only. Prints a TSV of every path where `HEAD` differs from the recorded
upstream commit, sorted by category:

| category | meaning |
|---|---|
| `duo-only` | file added by Duo; never conflicts |
| `upstream-hook` | upstream file Duo edits; a sync hotspot |
| `branding` | upstream file Duo replaces wholesale (identity, icons, About page) |

```sh
tools/duo/duo-touched-ledger.sh > .upstream/duo-touched.tsv     # refresh the ledger
tools/duo/duo-touched-ledger.sh --check .upstream/duo-touched.tsv
```

`--check` exits 1 when `HEAD` edits an upstream-owned file that the committed
ledger does not list. Run it before every release (or in CI). A failure means a
change added a new upstream hotspot: move the code into a Duo-owned file and keep
a one-line hook, or, if the edit is unavoidable, regenerate the ledger and add an
entry to `.upstream/DIVERGENCES.md` when it is a deliberate divergence.

The upstream commit must be present locally (`git fetch origin <sha>`).

### `--hotspots`: how big is each hook, not just whether one is new

`--check` only catches a *new* upstream-owned file being touched; it says nothing
about an existing hook growing over time, which is exactly what makes a file
risky at the next sync. `--hotspots` sizes every `upstream-hook` file instead:

```sh
tools/duo/duo-touched-ledger.sh --hotspots            # every upstream-hook file, added lines desc
tools/duo/duo-touched-ledger.sh --hotspots 10         # just the top 10
tools/duo/duo-touched-ledger.sh --hotspots --max-added 60
```

Plain `--hotspots [N]` only prints (`added<TAB>deleted<TAB>path`, sorted by added
lines, limited to the top `N` rows when given) and always exits 0 -- a quick "what
grew" check, by hand or before writing a hook. Adding `--max-added N` turns it into
a gate: exit 1, with every offending file listed on stderr, when an upstream-hook
file's added lines (vs the recorded upstream commit) exceed `N`.

A file can out-grow a shared `N` for a reason already accepted -- a big hook that
genuinely needs to stay big, not a new one to go fix. List it in
`.upstream/hook-budget.tsv` (`path<TAB>max_added<TAB>reason`, tab-separated, one
line per file) and that file's own `max_added` wins over `N` for it alone; `--check`
is unaffected (it only ever cares whether a path is new, never its size).
`--allowlist FILE` points at a different budget file (the default is
`.upstream/hook-budget.tsv`, silently skipped when it does not exist).

This is a hand/CI check, run deliberately with whatever `N` the caller picks for
that run -- it is **not** wired into `hooks/pre-commit` or any other git hook, and
should not be.

## `duo-upstream-sync.sh`

```sh
tools/duo/duo-upstream-sync.sh --dry-run <upstream-commit>   # predict conflicts only
tools/duo/duo-upstream-sync.sh <upstream-commit>             # merge on a new branch
```

`--dry-run` writes nothing. It lists the predicted textual conflicts (using the
recorded upstream commit as an explicit merge base) and the upstream files that
changed and that Duo also edits; check those for API changes even when they merge
cleanly.

Without `--dry-run` it requires a clean tree, then:

1. creates `sync/eden-<sha>` from `HEAD` (adding a tree-unchanged bridge commit if
   the recorded upstream commit is not yet an ancestor);
2. enables `rerere` and adds `merge=ours` rules for Duo-owned branding files to
   `.git/info/attributes` (local only; no tracked file changes);
3. merges the upstream commit without committing, removes upstream AI-agent
   files, and updates `source_commit` in `.upstream/eden.json`;
4. lists the conflicts left to resolve.

Then resolve the conflicts, fill `release_tag` / `release_url` in
`.upstream/eden.json`, build, run the tests, review `.upstream/DIVERGENCES.md`
(drop entries upstream has fixed itself), refresh the ledger and commit.

Formatting rule: only format Duo-owned paths. Never run `clang-format` over whole
upstream files; reflowed upstream code conflicts on every sync.

## `daybreak-texture-check.py`

Not a sync tool: the pixel check from PR #12 for Trails through Daybreak II rendering
(stale or missing textures). It needs your own 1920x1080 captures of the scenes named in
its header and Pillow (plus ffmpeg for the video modes); it is not run by any test suite.
