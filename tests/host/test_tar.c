#include "test.h"
#include "fs/tar.h"
#include <stdlib.h>

TEST(tar_octal_fields) {
    CHECK_EQ_INT(tar_parse_octal("00000000012", 12), 10);
    CHECK_EQ_INT(tar_parse_octal("0000755\0", 8), 0755);
    CHECK_EQ_INT(tar_parse_octal("   755 \0", 8), 0755);
    CHECK_EQ_INT(tar_parse_octal("\0\0\0\0", 4), 0);
    CHECK_EQ_INT(tar_parse_octal("777", 2), 077); /* never reads past n */
    CHECK_EQ_INT(tar_parse_octal("77777777777", 11), 8589934591LL);
}

/* The header of a real archive written by the host's tar. */
TEST(tar_header_from_host_tar) {
    char dir[] = "/tmp/atos-tar-XXXXXX";
    if (!mkdtemp(dir)) {
        CHECK(!"mkdtemp failed");
        return;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "cd %s && printf 'hello tar\\n' > f.txt && tar --format=ustar -cf a.tar f.txt", dir);
    if (system(cmd) != 0) {
        printf("    (skipped: host tar unavailable)\n");
        return;
    }
    snprintf(cmd, sizeof(cmd), "%s/a.tar", dir);
    FILE *f = fopen(cmd, "rb");
    CHECK(f != NULL);
    if (!f) return;
    struct tar_header h;
    CHECK_EQ_INT(fread(&h, 1, sizeof(h), f), TAR_BLOCK);
    fclose(f);

    CHECK_EQ_INT(sizeof(struct tar_header), TAR_BLOCK);
    CHECK(tar_checksum_ok(&h));
    CHECK_EQ_STR(h.name, "f.txt");
    CHECK_EQ_INT(tar_parse_octal(h.size, sizeof(h.size)), 10);
    CHECK(h.typeflag == TAR_TYPE_FILE);
    CHECK(memcmp(h.magic, "ustar", 5) == 0);

    h.name[0] ^= 1; /* any corruption breaks the checksum */
    CHECK(!tar_checksum_ok(&h));

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}
