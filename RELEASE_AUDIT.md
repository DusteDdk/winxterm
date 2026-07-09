# Release Audit

This pass prepared the repository for public release by keeping the releasable
Windows project as the single project directory and removing local-only
artifacts.

## Structure Decisions

- The old top-level `resources/` directory was folded into `resources/` so
  resource inputs live with the CMake project that consumes them.
- The outer duplicate `README.md` was merged into `README.md`.
- Root Markdown metadata was moved into this project directory so the parent
  directory contains only `winxterm/`.
- The deleted historical `xterm-410/`, `winxterm-410/`, and `xterm.tar.gz`
  entries remain absent from the working tree. This public tree keeps only the
  current Windows project and its required vendored dependencies/assets.

## Removed From Public Contents

- CMake/MSVC build trees: `build/`, `build-fast-release/`.
- Release binary output: `dist-fast-release/`.
- Nested/editor metadata: `.git/`, `.cursor/`, `.vs/`, `.vscode/`.
- Local scratch/test files: `.testfile.txt.swp`, `test.bmp`, `testfile.txt`,
  and other scratch files.
- Captured developer environment: `resources/env.txt`.

## Private-Information Scan

The removed `env.txt` contained a developer username, corporate OneDrive path,
installed-tool PATH entries, and local environment details. The Ctrl+R concept
document was sanitized to use generic example usernames, paths, and credential
text. Source fixtures that used a personal Windows path were changed to generic
example paths.

No credential material or private key material remains in the scanned release
tree.

## Licensing Scan

The licensing inventory is in `THIRD_PARTY_NOTICES.md`.
