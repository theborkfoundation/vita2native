# SQLite 3.11.1 amalgamation

Vendored from <https://www.sqlite.org/2016/sqlite-amalgamation-3110100.zip>.

The 3.60 firmware ships this API generation, so the exported `SceSqlite` NIDs
match without shims. SQLite is in the public domain; see
<https://www.sqlite.org/copyright.html>.

Only `sqlite3.c` and `sqlite3.h` are used. Compiler warnings are suppressed for
the amalgamation in `CMakeLists.txt`.
