# Testing in Proven

## Regression Tests
Regression tests are not general feature tests.
They encode specific bugs that were previously found and fixed.

Phase tests check broad feature behavior.
Regression tests check that known dangerous edge cases do not return.

## Test Structure
- `tests/test_phase*.c`: Feature-specific behavior and API coverage.
- `tests/test_regression_*.c`: Narrow focus on previously identified and resolved issues.
