#include "test.h"
#include "grep/regex.h"

#define MATCH(re, text)    CHECK(regex_search(re, text, 0) == 1)
#define NO_MATCH(re, text) CHECK(regex_search(re, text, 0) == 0)

TEST(regex_literals_and_anchors) {
    MATCH("abc", "xxabcxx");
    NO_MATCH("abd", "xxabcxx");
    MATCH("^abc", "abcdef");
    NO_MATCH("^abc", "xabc");
    MATCH("def$", "abcdef");
    NO_MATCH("def$", "defx");
    MATCH("^$", "");
    NO_MATCH("^$", "x");
    MATCH("", "anything");
}

TEST(regex_repetition) {
    MATCH("ab*c", "ac");
    MATCH("ab*c", "abbbc");
    NO_MATCH("ab+c", "ac");
    MATCH("ab+c", "abbc");
    MATCH("colou?r", "color");
    MATCH("colou?r", "colour");
    NO_MATCH("colou?r", "colouur");
    MATCH("^a.*z$", "abcz");
    MATCH("^.*$", "");
    MATCH("a*a*a*a*b", "aaaaaaaaaab"); /* backtracking, not exponential on success */
}

TEST(regex_sets_and_escapes) {
    MATCH("^1[0-2]3$", "113");
    NO_MATCH("^1[0-2]3$", "133");
    MATCH("[^0-9]", "12a4");
    NO_MATCH("^[^0-9]+$", "12a4");
    MATCH("[]x]", "]");
    MATCH("[a-]", "-");
    MATCH("\\d+\\.\\d", "v1.2");
    NO_MATCH("a\\.b", "axb");
    MATCH("\\w+ \\w+", "hello world");
    MATCH("\\$5", "cost: $5");
}

TEST(regex_case_folding) {
    CHECK(regex_search("initrd", "The INITRD file", 1) == 1);
    CHECK(regex_search("initrd", "The INITRD file", 0) == 0);
    CHECK(regex_search("[a-c]+", "XBX", 1) == 1);
}

TEST(regex_check_errors) {
    CHECK(regex_check("a*b+c?") == NULL);
    CHECK(regex_check("^abc$") == NULL);
    CHECK(regex_check("*a") != NULL);
    CHECK(regex_check("a**") != NULL);
    CHECK(regex_check("[abc") != NULL);
    CHECK(regex_check("abc\\") != NULL);
}
