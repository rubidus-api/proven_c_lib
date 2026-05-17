# Coding Style

## C Standard
*   Adhere strictly to **C23**.
*   Custom types must be used, e.g. `proven_size_t`, `proven_ptrdiff_t`.

## Naming Conventions
*   **Variables/Functions**: `lower_snake_case`
*   **Types**: `suffix_t` (e.g., `proven_mem_t`)
*   **Macros**: `UPPER_SNAKE_CASE`

## Resource Management
*   Standard C `goto cleanup;` pattern for non-memory system resources to enforce Single-Entry Single-Exit (SESE). No non-standard `defer`.
