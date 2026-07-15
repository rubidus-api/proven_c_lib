# T002: Operating File Role Guidance

## Requirement

Agents can identify the intended role of `SPEC.md`, `REQUIREMENTS.md`, and `docs/tests/test-index.md` without reading broad history.

## Verification Method

Run `scripts/project-check.sh`.

## Assertions

- `docs/operations/README.md` describes `SPEC.md` as the current behavior, architecture, and layout contract.
- `docs/operations/README.md` describes `REQUIREMENTS.md` as current accepted requirements and constraints when present.
- `docs/operations/README.md` describes `docs/tests/test-index.md` as the compact process/TDD catalog.
- `docs/tests/test-index.md` stays compact and links detailed cases under `docs/tests/cases/`.
