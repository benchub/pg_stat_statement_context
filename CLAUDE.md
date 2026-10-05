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
3. **Build.** Launch a *builder* agent (`general-purpose`, claude-opus-5.5) with the full item text, the relevant `DESIGN.md` sections, and these rules. The builder writes the failing test first, then the implementation.
4. **Adversarial review.** Hand the builder's changes to a *different* agent that runs on a different model (e.g. `code-review` with gpt-6.1-sol). Its job is to find real bugs, gaps in tests, vacuous tests, and deviations from the design.
5. **Iterate.** Send any findings back to the same builder agent to fix (use `write_agent`), then review again. Allow at most **2** build→review rounds.
   - If a review finds nothing, land the item.
   - If findings remain after 2 rounds, land the work that is complete and passing. Split the rest into one or more new backlog items, with dependencies and questions noted.
6. **Land.** Commit directly to `main` with a message that references the item ID.
7. **Archive.** Move the finished item from `BACKLOG.md` to `BACKLOG-COMPLETE.md`. Update the summary table and the statuses of any items that depended on it.

## 3. Backlog conventions

- Task IDs use the format `YYYYMMDD-HHMMSS-N` (e.g. `20261005-091023-1`). Use the time you create the item, and number sequentially within that batch.
- Every item has a description, acceptance criteria, `Depends on`, `Open questions`, and `Status` (`ready` / `blocked-on-deps` / `blocked-on-questions`).
- Keep dependencies acyclic, and only reference IDs that exist (in either backlog file).

## 4. Questions

You can ask the user clarifying questions about any item at any time. Asking is better than guessing on design decisions.

## 5. Concurrency

- By default, work on **one** backlog item at a time to avoid merge conflicts.
- You may run items in parallel only if they are clearly independent: they touch disjoint files or modules and are unlikely to conflict.

## 6. Sandbox

- Keep all file operations inside this repository directory, **including temporary files**. Use a git-ignored scratch directory such as `./tmp/`, not `/tmp` or `$TMPDIR`.
- You may use Docker to build the extension and test it against the supported PostgreSQL versions.
