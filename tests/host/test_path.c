#include "test.h"
#include "lib/path.h"
#include <atos/abi.h>

static void expect(const char *in, const char *want) {
    char out[PATH_MAX_LEN];
    int err = path_normalize(in, out);
    CHECK_EQ_INT(err, 0);
    if (!err) CHECK_EQ_STR(out, want);
}

TEST(path_normalize_canonical_forms) {
    expect("/", "/");
    expect("//", "/");
    expect("/a", "/a");
    expect("/a/", "/a");
    expect("//a//b///", "/a/b");
    expect("/a/./b/.", "/a/b");
    expect("/a/b/../c", "/a/c");
    expect("/..", "/");
    expect("/../../a", "/a");
    expect("/a/../../b", "/b");
    expect("/a/b/c/../../..", "/");
    expect("/.hidden/..x/...", "/.hidden/..x/...");
    expect("/disk/docs/A long file name.txt", "/disk/docs/A long file name.txt");
}

TEST(path_normalize_errors) {
    char out[PATH_MAX_LEN];
    CHECK_EQ_INT(path_normalize("", out), -ENOENT);
    CHECK_EQ_INT(path_normalize("a/b", out), -ENOENT);

    char name[ATOS_NAME_MAX + 2];
    name[0] = '/';
    memset(name + 1, 'x', ATOS_NAME_MAX - 1);
    name[ATOS_NAME_MAX] = '\0'; /* component of ATOS_NAME_MAX-1 chars: fits */
    CHECK_EQ_INT(path_normalize(name, out), 0);
    name[ATOS_NAME_MAX] = 'x';
    name[ATOS_NAME_MAX + 1] = '\0'; /* one more: too long */
    CHECK_EQ_INT(path_normalize(name, out), -ENAMETOOLONG);

    /* Total length: 5 * "/" + 50 chars = 255 bytes fits with the NUL... */
    char big[400] = "";
    for (int i = 0; i < 5; i++) {
        strcat(big, "/");
        for (int k = 0; k < 50; k++) strcat(big, "y");
    }
    CHECK_EQ_INT(path_normalize(big, out), 0);
    strcat(big, "/z"); /* ...and 257 does not */
    CHECK_EQ_INT(path_normalize(big, out), -ENAMETOOLONG);
    /* but ".." can bring a long input back under the limit */
    strcat(big, "/..");
    CHECK_EQ_INT(path_normalize(big, out), -ENAMETOOLONG);
}
