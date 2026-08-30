# Contributing to Studio Duo

Studio Duo welcomes focused contributions that move the accepted design toward
a reliable cross-platform DAW.

## Before coding

1. Read the [product and technical design](design.md).
2. Keep the real-time rules non-negotiable: no allocation, locks, file access,
   logging, parsing, or blocking IPC on the audio callback.
3. Open an issue before large architectural changes or new dependencies.

## Development workflow

```sh
cmake -S . -B build -DSTUDIO_DUO_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

Keep changes narrow, use typed project commands for user-visible edits, and add
tests for model, persistence, or engine behavior. Audio fixtures must be
generated or redistributable and must not contain customer or personal data.

## Pull requests

- Explain the user-facing behavior and architectural impact.
- Include macOS and Windows implications.
- State how real-time safety was preserved.
- Update documentation when setup, behavior, formats, or architecture change.
- Keep commits free of generated build output.

By contributing, you agree that your contribution is licensed under
AGPL-3.0-only.
