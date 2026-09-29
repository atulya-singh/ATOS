/* poweroff: turns the machine off through ACPI. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    reboot(ATOS_REBOOT_POWEROFF);
    printf("poweroff: %s\n", strerror(errno));
    return 1;
}
