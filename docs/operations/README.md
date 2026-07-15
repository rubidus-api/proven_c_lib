# Operations

Project workflow and automation notes for `proven_c_lib`.

This directory is for working rules, document update rules, and local verification
policy. It is not public API documentation; public user-facing documentation
remains in `README.md`, `manual/`, `CHANGELOG.md`, and public headers.

## Standard Flow

1. Read `AGENTS.md` and `CONTEXT.md` for current local state.
   For task selection or resume work, also read `BACKLOGS.md` and `HANDOFF.md`.
2. Identify the smallest source, test, and documentation scope.
3. Update tests before behavior changes when practical.
4. Run the narrowest relevant `./nob` command, then broader checks as risk grows.
5. Run `scripts/project-check.sh` before reporting or committing.

## Automation Scripts

- Put repeated mechanical work under `scripts/` instead of re-explaining it in every AI session.
- Scripts should be deterministic, narrow, and cheap to run. Prefer `--help`, dry-run modes, and explicit failure messages when adding new tools.
- Linux x86_64 is the baseline runtime. Windows 11 support is via Python 3 plus Git Bash/WSL unless a native PowerShell wrapper is added.
- If a required program is missing, report the exact missing tool and stop instead of improvising.
- Use scripts to save tokens for mechanical checks, generated indexes, linting, and bootstrap validation.

## Document Update Rules

- Update `README.md` for public install, usage, build, API, or project-structure changes.
- Update `CHANGELOG.md` for notable public source, test, build, or documentation changes.
- Update `TEST.md` when verification commands or coverage expectations change.
- Update `manual/` when public API behavior, parameters, examples, or misuse cases change.
- Keep `SPEC.md` as the local current behavior, architecture, and layout contract. Do not use it as a chronology or rejected-ideas archive.
- Keep `REQUIREMENTS.md`, when present, as the local current accepted requirements and constraints. Put requirement history or conflicts in a dedicated history file, not in the current requirements file.
- Keep `docs/tests/test-index.md` as a compact process/TDD catalog. Detailed cases belong under `docs/tests/cases/`; full product verification remains in root `TEST.md` and executable tests under `tests/`.
- Keep local planning and handoff notes in ignored files. Use `BACKLOGS.md` for the compact current queue, `HANDOFF.md` for the current resume packet, and `TODO.md` only as legacy detailed backlog input.

## Changelog Rules

- Follow Keep a Changelog 2.0.0 style unless the user asks to refresh from `https://keepachangelog.com/`.
- Use `# Changelog`, a short preamble, and `## [Unreleased]` at the top.
- Release sections use `## [x.y.z] - YYYY-MM-DD` in newest-first order.
- Group changes under `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, and `Security`.
- Write notable user/developer impact, not raw commit logs. Mark breaking changes clearly.
- Keep version headings linkable with compare/release links when the project has tags.
