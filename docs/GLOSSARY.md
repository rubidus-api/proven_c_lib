# Glossary

*   **CRT**: C Runtime Library. `proven` focuses on stdio-minimized behaviors and confines required standard C library abstractions explicitly inside the Platform Access Layer (`PAL`).
*   **OOB**: Out-of-bounds error.
*   **UB**: Undefined Behavior. The library attempts to reduce common UB risks through checked arithmetic, explicit bounds, and tests.
*   **VTable Allocator**: An interface struct capturing function pointers for memory allocations.
*   **MPMC**: Multi-Producer Multi-Consumer (used in job scheduler).
