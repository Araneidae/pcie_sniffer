/* Archiver program for capturing data from FA sniffer and writing to disk.
 * Also makes the continuous data stream available over a dedicated socket. */

#include <stdbool.h>
#include <stdio.h>

#include "error.h"
#include "buffer.h"
#include "sniffer.h"
#include "disk_writer.h"


#define K               1024
#define FA_BLOCK_SIZE   (64 * K)    // Block size for device read
#define FA_BLOCK_COUNT  (2 * K)     // 128 MB seems enough and manageable

#define DISK_FILE       "/scratch/writer.out"


int main(int argc, char **argv)
{
    bool ok =
        TEST_OK(initialise_buffer(FA_BLOCK_SIZE, FA_BLOCK_COUNT))  &&
        TEST_OK(initialise_sniffer("/dev/fa_sniffer0"))  &&
        TEST_OK(initialise_disk_writer(DISK_FILE));

    if (ok)
    {
        printf("running\n");
        char line[80];
        printf("> ");
        fflush(stdout);
        fgets(line, sizeof(line), stdin);

        terminate_sniffer();
        terminate_disk_writer();
        printf("done\n");
    }
}
