#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* calc [expression]: evaluates 64-bit integer arithmetic with C's
 * operators and precedence -- ( ) unary - ~ !, * / %, + -, << >>, &, ^, |
 * -- on decimal, 0x hex, and 0b binary literals, printing the result in
 * decimal and hex. Without arguments, evaluates one expression per line
 * of standard input. (Integer-only: ATOS user code has no FPU state.) */

static const char *p;
static const char *error;

static void skip(void) {
    while (*p == ' ' || *p == '\t') p++;
}

static long long expr(void);

static long long primary(void) {
    skip();
    if (*p == '(') {
        p++;
        long long v = expr();
        skip();
        if (*p != ')') { if (!error) error = "missing )"; return 0; }
        p++;
        return v;
    }
    if (*p == '-') { p++; return -primary(); }
    if (*p == '+') { p++; return primary(); }
    if (*p == '~') { p++; return ~primary(); }
    if (*p == '!') { p++; return !primary(); }
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
    unsigned long long v = 0;
    int digits = 0;
    for (;; p++, digits++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (!digits && !error) error = *p ? "unexpected character" : "unexpected end of expression";
    return (long long)v;
}

/* Precedence climbing over binary operators, loosest first. */
static int prec(const char *s, int *len) {
    *len = 2;
    if (s[0] == '<' && s[1] == '<') return 5;
    if (s[0] == '>' && s[1] == '>') return 5;
    *len = 1;
    switch (s[0]) {
    case '|': return 1;
    case '^': return 2;
    case '&': return 3;
    case '+': case '-': return 6;
    case '*': case '/': case '%': return 7;
    }
    return 0;
}

static long long binary(int min_prec) {
    long long lhs = primary();
    for (;;) {
        skip();
        int len, pr = prec(p, &len);
        if (!pr || pr < min_prec) return lhs;
        char op = *p;
        p += len;
        long long rhs = binary(pr + 1);
        switch (op) {
        case '|': lhs |= rhs; break;
        case '^': lhs ^= rhs; break;
        case '&': lhs &= rhs; break;
        case '<': lhs = (long long)((unsigned long long)lhs << (rhs & 63)); break;
        case '>': lhs >>= (rhs & 63); break;
        case '+': lhs = (long long)((unsigned long long)lhs + (unsigned long long)rhs); break;
        case '-': lhs = (long long)((unsigned long long)lhs - (unsigned long long)rhs); break;
        case '*': lhs = (long long)((unsigned long long)lhs * (unsigned long long)rhs); break;
        case '/':
        case '%':
            if (rhs == 0) { if (!error) error = "division by zero"; return 0; }
            if (rhs == -1) lhs = op == '/' ? (long long)(0 - (unsigned long long)lhs) : 0;
            else lhs = op == '/' ? lhs / rhs : lhs % rhs;
            break;
        }
    }
}

static long long expr(void) { return binary(1); }

static int evaluate(const char *text) {
    p = text;
    error = NULL;
    long long v = expr();
    skip();
    if (!error && *p) error = "unexpected character";
    if (error) {
        printf("calc: %s at '%s'\n", error, p);
        return 1;
    }
    printf("%lld (0x%llx)\n", v, (unsigned long long)v);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        char text[512];
        size_t len = 0;
        for (int i = 1; i < argc; i++) {
            size_t n = strlen(argv[i]);
            if (len + n + 2 > sizeof(text)) {
                printf("calc: expression too long\n");
                return 2;
            }
            memcpy(text + len, argv[i], n);
            len += n;
            text[len++] = ' ';
        }
        text[len] = '\0';
        return evaluate(text);
    }
    char line[512];
    size_t len = 0;
    int status = 0;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c != '\n') {
            if (len < sizeof(line) - 1) line[len++] = c;
            continue;
        }
        line[len] = '\0';
        if (len) status |= evaluate(line);
        len = 0;
    }
    return status;
}
