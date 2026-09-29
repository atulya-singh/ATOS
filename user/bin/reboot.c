/* reboot: restarts the machine. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    reboot(ATOS_REBOOT_RESTART);
    printf("reboot: %s\n", strerror(errno));
    return 1;
}
