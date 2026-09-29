#pragma once

/* A small backtracking regular-expression matcher: literal characters,
 * . (any), [set] and [^set] with ranges, * + ? on the previous atom,
 * ^ and $ anchors, and \ to escape. Returns 1 if `re` matches anywhere in
 * `text`. `icase` folds ASCII case. */
int regex_search(const char *re, const char *text, int icase);

/* 0 if `re` is well formed, else a static message describing the error. */
const char *regex_check(const char *re);
