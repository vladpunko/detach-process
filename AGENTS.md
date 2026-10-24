# Rules

- This is a systems utility: prioritize correctness.
- Prevent hangs, leaks, and unintended data loss.
- Handle all edge cases and weak points.
- Keep startup fast and changes minimal.
- Use C99 for the utility and C++20 for tests.
- Verify code changes on Linux and macOS.
- Always use subagents to review edge cases and errors.
- Reproduce findings and add regression tests.
- Keep 100% line, function, and branch coverage of `detach.c`.
- Never weaken tests or coverage checks.
- Preserve documented behavior and existing code style.
- Commit each completed atomic change after relevant checks and tests pass.
- Use short, single-line commit messages.

## Make commands

- `make` or `make build`: release build for manual checks
- `make tests`: run tests during development
- `make coverage`: final check, including the build, tests, and full coverage
