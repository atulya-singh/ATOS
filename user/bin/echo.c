#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) write(STDOUT_FILENO, " ", 1);
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
    }
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}
