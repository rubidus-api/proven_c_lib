# Security

## Thread Safety Contract
For maximum single-thread efficiency, the library minimizes internal locking. Core algorithms do not silently employ mutexes or spinlocks where unnecessary. You are responsible for managing internal thread barriers across structures like `proven_array_t`.

## Error Checking
Functions must explicitly handle Result returns. Do not rely on macro exceptions for early returns. Partial-free logic is disallowed; arenas must drop entire contexts safely instead.

## Unclear Security Vectors
*   **Filesystem Context:** Path traversal and symlink vulnerabilities represent external logical risks. The `proven` runtime bounds path formatting memory limits, but consumers must invoke explicit sanitation logic before executing PAL file opening requests.
