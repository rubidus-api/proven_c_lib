# Project Working Guidelines: `proven` Library

**Current Version:** `v26.04.24`

## 0. Top Priority: Documentation Language
*   **MANDATORY:** All project documentation, specifications, manuals, test logs, and code comments **MUST be written strictly in English**.
*   **Enforcement:** Any new documentation or updates to existing files must be reviewed for language consistency. This is to ensure global accessibility and alignment with international open-source standards.

## 1. Specification & Documentation First
*   **Rule:** Every design change or new feature request begins with the Specification (`/SPEC.md`) and the User Manual (`/manual/`).
*   **Process:**
    1.  Update `SPEC.md` to reflect architectural changes.
    2.  Update relevant documentation in `/manual/*.md` simultaneously.
    3.  Ensure all changes maintain the "Strict Bottom-Up" and "No-CRT" principles.

## 2. Technical Standard: C23 & Portability
*   **Modern C:** Strictly adhere to the **C23 standard**. Leverage C23 features (e.g., `[[nodiscard]]`, `stdckdint.h`, `_BitInt`) while ensuring strict compatibility with freestanding environments.
*   **Type System Integrity:**
    *   **Custom Types:** Define and use custom types like `proven_u8`, `proven_i32`, `proven_size_t` to isolate the core from the external environment and clarify intent. Avoid direct use of primitive types (`int`, `long`) and conduct rigorous reviews of range and signedness.
    *   **Index & Size:** Strictly distinguish between `proven_size_t` for data size and indexing, and `proven_ptrdiff_t` (based on `ptrdiff_t`) for pointer differences or offsets.
*   **Naming Conventions:**
    *   **Identifiers:** Use lower snake case (`snake_case`) for variable and function names.
    *   **Typedefs:** All explicitly defined type names (including structs and enums) MUST end with the `_t` suffix (e.g., `proven_mem_t`). Basic fixed-width integer types (e.g., `proven_u8`) are exempt from the `_t` suffix to maintain brevity and align with project style.
    *   **Macros:** All macros MUST use upper snake case (`UPPER_SNAKE_CASE`).
*   **Portability:** Proactively address C standard portability issues. Avoid implementation-defined behavior unless explicitly wrapped in the platform abstraction layer.
*   **Safety & UB:** Conduct rigorous reviews to eliminate **Undefined Behavior (UB)**. Every memory access and arithmetic operation must be verified for safety.

## 3. Memory Model & Performance
*   **Strict Aliasing & Provenance:** Always consider provenance rules and strict aliasing for optimized, bug-free code.
*   **Provenance-Aware Types:** Use `proven_byte_t` and `proven_mem_t` to ensure the compiler can optimize safely without violating memory rules.
*   **Low-Level Optimization:** 
    *   **Memory & Cache:** Ensure proper memory alignment and data locality to maximize CPU cache hits.
    *   **CPU Efficiency:** Optimize for register utilization and minimize branch mispredictions by reducing complex conditional branching where possible.
    *   **Zero-Overhead:** Code must be as efficient as a manual implementation while remaining bug-free and safe.

## 4. Error Handling & Resource Management
*   **Value-Return (Result Pattern):** Functions that can fail MUST return a `Result` type structure (e.g., `proven_result_mem_mut_t`) containing both the error code (`proven_err_t`) and the value, rather than using output pointers.
*   **Explicit Control Flow:** Do NOT use magic macros (like `PROVEN_TRY`) to hide early returns. Error propagation MUST be explicit using standard `if (res.err != PROVEN_OK)` checks to maintain code visibility and debuggability.
*   **No Partial Free:** Memory allocations are managed strictly by the Arena. Never attempt to `free` or rollback memory on error.
*   **Resource Cleanup:** For non-memory system resources (files, sockets), use the standard C `goto cleanup_xxx;` pattern to enforce a Single-Entry Single-Exit (SESE) strategy. Do NOT rely on non-standard `defer` extensions.

## 5. Build System & Automation
*   **nob.h:** The project strictly uses `nob.h` (from tsoding) for its build system. `Makefile` or `CMakeLists.txt` are NOT allowed.
*   **Build Script:** The build script MUST be written in `nob.c` at the root of the project.
*   **Compilation:** To build the project, first compile the build script (`cc nob.c -o nob`), then run `./nob` to build the library and tests.

## 6. Development Workflow: TDD
*   **Test-Driven Development:** Implementation **MUST** follow the TDD cycle:
    1.  Write failing unit tests first in the `/tests/` directory as plain C source files. No external test frameworks are used; tests are compiled using `nob.c` and run manually.
    2.  Implement the minimum code to pass the tests.
    3.  Refactor while maintaining architectural integrity.
*   **Validation:** Use static analysis and runtime sanitizers to ensure UB-free implementation during the testing phase.

## 7. External Dependencies & Licensing
*   **Dependency Isolation (The Platform Layer):** Any inevitable reliance on the standard library (libc) or OS APIs MUST be strictly isolated into the `/platform/` abstraction layer (e.g., `proven_sys_mem.h`). The core library (`/src/proven/`) is FORBIDDEN from directly including `<stdlib.h>` or OS headers.
*   **Polymorphic VTable Allocator Constraint:** All core systems and object creation pipelines in the project MUST fundamentally rely on the single polymorphic `proven_allocator_t` trait. They must NOT tightly couple to specific implementations like Arena or Heap internally. Existing specific allocators (e.g., `proven_heap_allocator()`) must be implemented as vtable interface patterns that route strictly through the aforementioned `/platform/` layer.
*   **License:** The project is licensed under the **MIT License**.
*   **Compatibility:** Any external code/library MUST be strictly compatible with the MIT License. GPL-style copyleft or restrictive commercial licenses are forbidden.

## 8. Project structure
*   `/SPEC.md`: Root specification and design document.
*   `/AGENTS.md`: Core project working guidelines (this file).
*   `/manual/`: User and developer manuals.
*   `/include/`: Public headers (`.h`).
*   `/src/`: Library implementation files (`.c`).
*   `/tests/`: TDD unit tests and test runners.
*   `/platform/`: Platform abstraction layer (isolation of external syscalls/libs).

## 9. Documentation Language
*   **English Only:** All project documentation, including specifications (`SPEC.md`), test logs (`TEST.md`), architectural decisions, user manuals, and code comments, **MUST** be written strictly in English. This ensures global accessibility and consistency across the codebase.
*   **No Emojis or Pictograms:** When creating or updating documents, you MUST NOT include weird unicode emojis, pictograms, or symbol characters (e.g., 🚀, ⚡, ✅, etc.). Maintain a strictly professional text-based format.

## 10. Version Control and Updates
*   **Rule:** Whenever files are modified by AI through chat, the `include/proven/version.h` file MUST be updated with the current date (format: YY.MM.DD).
*   **Process:** Update `PROVEN_VERSION_STRING` (e.g., `"proven_c_lib-v26.04.24"`) and `PROVEN_VERSION_NUM` (e.g., `260424`) in `include/proven/version.h`.
*   **Documentation:** Synchronize the new version number (`v26.04.24`) into `SPEC.md`, `AGENTS.md`, `index.html`, and all Markdown manuals inside the `/manual/` folder.

## 11. Web Footer Mandate
*   **index.html Footer:** The footer section at the bottom of the screen in `index.html` MUST display the following information exactly:
    *   Creator ID: `rubidus-api`
    *   Email: `rubidus@gmail.com`
    *   GitHub URL: `https://github.com/rubidus-api/proven_c_lib/`
    *   License Information: MIT License
    *   *Example format:* `Creator: rubidus-api | Email: rubidus@gmail.com | GitHub: <a href="https://github.com/rubidus-api/proven_c_lib/">https://github.com/rubidus-api/proven_c_lib/</a> | License: MIT License`