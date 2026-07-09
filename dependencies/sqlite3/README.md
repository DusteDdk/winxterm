SQLite 3.53.3
=============

This directory vendors the SQLite amalgamation from:

https://sqlite.org/2026/sqlite-amalgamation-3530300.zip

Upstream SHA3-256:

`d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`

The SQLite download page does not provide a precompiled Windows static
library. Its Windows precompiled packages are DLLs or command-line tools, so
winxterm builds `sqlite3.c` directly into a CMake `STATIC` library to avoid a
companion `sqlite3.dll`.

The CMake target keeps the build intentionally basic by omitting loadable
extensions, deprecated APIs, and default memory allocation status tracking.
