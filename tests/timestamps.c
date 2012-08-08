/* Simple fa_sniffer test of timestamps. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <time.h>

#include "fa_sniffer.h"

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("test <block-size> <count>\n");
        return 1;
    }

    int block_size = atoi(argv[1]);
    int count = atoi(argv[2]);

    int fa = open("/dev/fa_sniffer0", O_RDONLY);
    if (fa == -1)
    {
        perror("open");
        return 2;
    }

    printf("ioctl version: %d\n", ioctl(fa, FASNIF_IOCTL_GET_VERSION));
    printf("entry count: %d\n", ioctl(fa, FASNIF_IOCTL_GET_ENTRY_COUNT));

    for (int i = 0; i < count; i ++)
    {
        char block[block_size];
        ssize_t rx = read(fa, block, block_size);
        struct fa_timestamp timestamp;
        int ctl = ioctl(fa, FASNIF_IOCTL_GET_TIMESTAMP, &timestamp);

        struct timespec now_ts;
        clock_gettime(CLOCK_REALTIME, &now_ts);
        uint64_t now =
            (uint64_t) now_ts.tv_sec * 1000000 + now_ts.tv_nsec / 1000;

        printf("%zd/%d => %"PRIu64".%06"PRIu64" / %"PRIu32" => %"PRIu64" us\n",
            rx, ctl,
            timestamp.timestamp / 1000000, timestamp.timestamp % 1000000,
            timestamp.residue, now - timestamp.timestamp);
    }
    return 0;
}
