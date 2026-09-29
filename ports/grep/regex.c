#include "regex.h"
#include <stddef.h>

static int fold(int c, int icase) {
    return icase && c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Length of the atom at re (a char, escape, ., or bracket set). */
static int atom_len(const char *re) {
    if (re[0] == '\\' && re[1]) return 2;
    if (re[0] == '[') {
        const char *p = re + 1;
        if (*p == '^') p++;
        if (*p == ']') p++; /* a leading ] is literal */
        while (*p && *p != ']') p++;
        return *p ? (int)(p - re) + 1 : -1;
    }
    return 1;
}

static int set_match(const char *set, int len, int c, int icase) {
    const char *p = set + 1, *end = set + len - 1;
    int negate = 0, hit = 0;
    if (*p == '^') { negate = 1; p++; }
    c = fold(c, icase);
    while (p < end) {
        int lo = fold((unsigned char)*p, icase);
        if (p + 2 < end && p[1] == '-') { /* a range; a trailing - is literal */
            int hi = fold((unsigned char)p[2], icase);
            if (c >= lo && c <= hi) hit = 1;
            p += 3;
        } else {
            if (c == lo) hit = 1;
            p++;
        }
    }
    return hit != negate;
}

static int atom_match(const char *re, int len, int c, int icase) {
    if (c == '\0') return 0;
    if (re[0] == '.') return 1;
    if (re[0] == '[') return set_match(re, len, c, icase);
    if (re[0] == '\\') {
        switch (re[1]) {
        case 'd': return c >= '0' && c <= '9';
        case 's': return c == ' ' || c == '\t';
        case 'w': return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_';
        default:  return fold(re[1], icase) == fold(c, icase);
        }
    }
    return fold((unsigned char)re[0], icase) == fold(c, icase);
}

static int match_here(const char *re, const char *text, int icase) {
    if (re[0] == '\0') return 1;
    if (re[0] == '$' && re[1] == '\0') return *text == '\0';
    int len = atom_len(re);
    char op = re[len];
    if (op == '*' || op == '+' || op == '?') {
        int min = op == '+' ? 1 : 0, max = op == '?' ? 1 : -1;
        /* Greedy: take as many as possible, then back off. */
        const char *t = text;
        int n = 0;
        while ((max < 0 || n < max) && atom_match(re, len, (unsigned char)*t, icase)) {
            t++;
            n++;
        }
        for (; n >= min; n--, t--) {
            if (match_here(re + len + 1, t, icase)) return 1;
        }
        return 0;
    }
    if (atom_match(re, len, (unsigned char)*text, icase)) return match_here(re + len, text + 1, icase);
    return 0;
}

int regex_search(const char *re, const char *text, int icase) {
    if (re[0] == '^') return match_here(re + 1, text, icase);
    do {
        if (match_here(re, text, icase)) return 1;
    } while (*text++);
    return 0;
}

const char *regex_check(const char *re) {
    if (*re == '^') re++;
    int have_atom = 0;
    while (*re) {
        if (*re == '*' || *re == '+' || *re == '?') {
            if (!have_atom) return "repetition operator with nothing before it";
            have_atom = 0;
            re++;
            continue;
        }
        if (*re == '$' && re[1] == '\0') break;
        int len = atom_len(re);
        if (len < 0) return "unterminated [";
        if (re[0] == '\\' && re[1] == '\0') return "trailing backslash";
        re += len;
        have_atom = 1;
    }
    return NULL;
}
