# Troubleshooting

*   **nob compilation errors**: Ensure C compiler is set correctly in env. (`cc nob.c -o nob`)
*   **Test Failures**: Tests are strictly C unit tools mapped in `nob.c` under `tests/`. Re-run to inspect exact assertion points.
*   **Linker and PAL Resolution:** If a `proven_sys_xxx` function is unresolved, ensure the `platform/` implementation files are compiled alongside the code. The core library intentionally leaves platform implementations undefined.
