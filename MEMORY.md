# proven_c_lib Memory

- Repository name: `proven_c_lib`.
- The project is a C23 systems library with explicit ownership, PAL-isolated hosted code, and a self-hosted `nob.c` build driver.
- The canonical version source is `include/proven/version.h`.
- Public docs and help text use `/home/user/work/...` examples only.
- The shared build root example is `/home/user/work/build/proven_c_lib`.
- `docs/` and `docs/ai/` markdown notes were consolidated into `AGENTS.md` and removed from the tree.
- Version bumps should be reflected in `README.md`, `SPEC.md`, `TEST.md`, `manual/`, `CHANGELOG.md`, and `docs-site/index.html` when present.
- The main verification modes are `build`, `release`, `strict`, `strict-error`, `asan`, `ubsan`, `tsan`, `regression`, `regression-asan`, `regression-ubsan`, `freestanding`, and `cross`.
