# CLAUDE.md — Working process for this repo

The design is in `DESIGN.md`. Work items are in `BACKLOG.md`, and finished items are in `BACKLOG-COMPLETE.md`.

## 1. Test-driven development (mandatory)

- Before you build a feature or fix a bug, there must be at least one **non-vacuous, failing** test for it. A test is non-vacuous if it exercises the new behavior and would only pass if the behavior is implemented correctly.
- Run the test and confirm it fails *for the expected reason* before you write any implementation.
- An item is not complete until that test, and the rest of the existing suite, passes.
- Tests must pass on every PostgreSQL version that `DESIGN.md` supports (currently PG 14–18).

## 2. Backlog workflow

For each backlog item:

1. **Validate.** Check that the item still makes sense against the current code and `DESIGN.md`, and that all its dependencies are finished. If it is stale, update or remove it first.
2. **Resolve open questions.** If the item lists open questions, ask the user with the `ask_user` tool before starting. Record the answers in the item (and in `DESIGN.md` if they change the design).
3. **Build.** Create the item's own worktree (see §7), then launch a *builder* agent (`general-purpose`, claude-opus-5.5) with the full item text, the relevant `DESIGN.md` sections, the worktree path, and these rules. The builder works only in its worktree. It writes the failing test first, then the implementation, and commits to the item's branch.
4. **Adversarial review.** Hand the builder's changes (`git diff main...<branch>` in its worktree) to a *different* agent that runs on a different model (e.g. `code-review` with gpt-6.1-sol). Its job is to find real bugs, gaps in tests, vacuous tests, and deviations from the design.
5. **Iterate.** Send any findings back to the same builder agent to fix (use `write_agent`), then review again. Allow at most **2** build→review rounds.
   - If a review finds nothing, land the item.
   - If findings remain after 2 rounds, land the work that is complete and passing. Split the rest into one or more new backlog items, with dependencies and questions noted.
6. **Land.** Rebase the item's branch onto `main`, resolve any conflicts, re-run the tests, then fast-forward `main` to it (the commit message references the item ID). Remove the worktree and delete the branch afterwards.
7. **Archive.** Run `scripts/backlog-complete.py <ID>`. It moves the item to `BACKLOG-COMPLETE.md`, removes its row from the summary table, and recomputes the statuses of the remaining items.

## 3. Backlog conventions

- Task IDs use the format `YYYYMMDD-HHMMSS-N` (e.g. `20261005-091023-1`). Use the time you create the item, and number sequentially within that batch.
- Every item has a description, acceptance criteria, `Depends on`, `Open questions`, and `Status` (`ready` / `blocked-on-deps` / `blocked-on-questions`).
- Keep dependencies acyclic, and only reference IDs that exist (in either backlog file).

## 4. Questions

You can ask the user clarifying questions about any item at any time. Asking is better than guessing on design decisions.

## 5. Concurrency

- By default, work on **one** backlog item at a time to avoid merge conflicts.
- You may run items in parallel only if they are clearly independent: they touch disjoint files or modules and are unlikely to conflict.
- Parallel or not, each builder works in its own worktree (§7), never in the main checkout.

## 6. Sandbox

- Keep all file operations inside this repository directory, **including temporary files**. Use a git-ignored scratch directory such as `./tmp/`, not `/tmp` or `$TMPDIR`.
- You may use Docker to build the extension and test it against the supported PostgreSQL versions. Files written inside a container's own ephemeral filesystem are fine. Anything written to the host through a bind mount must stay under `./tmp/`.

## 7. Workspaces

- Every builder gets its own git worktree and branch, inside this repository so it stays within the sandbox:
  `git worktree add worktrees/<ID> -b item/<ID> main`
  (`worktrees/` is git-ignored).
- The builder edits, builds, tests and commits only inside `worktrees/<ID>`. It never touches the main checkout or another item's worktree. Its temporary files go under `worktrees/<ID>/tmp/`.
- The main checkout belongs to the coordinator. Use it for `BACKLOG.md`, `BACKLOG-COMPLETE.md`, `DESIGN.md` and `CLAUDE.md`, and for landing branches. Make `DESIGN.md` updates for an item on its branch before landing, or as a separate commit on `main`.
- Reviewers read the branch diff (`git -C worktrees/<ID> diff main...HEAD`) and do not modify files.
- Docker test runs started from a worktree mount that worktree, so each item tests exactly its own code; no snapshots are needed.
