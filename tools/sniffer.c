/* Code to interface to fa_sniffer device. */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#include "error.h"
#include "buffer.h"

#include "sniffer.h"


static pthread_t sniffer_id;

static const char *fa_sniffer_device;



static void * sniffer_thread(void *context)
{
    int fa_sniffer;
    while (TEST_IO_(
        fa_sniffer = open(fa_sniffer_device, O_RDONLY),
        "Can't open sniffer device %s", fa_sniffer_device))
    {
        while (true)
        {
            void *buffer = get_write_block();
            if (buffer == NULL)
            {
                log_message("sniffer unable to write block");
                break;
            }
            bool gap =
                read(fa_sniffer, buffer, fa_block_size) <
                    (ssize_t) fa_block_size;
            release_write_block(gap);
            if (gap)
            {
                log_message("unable to read block");
                break;
            }
        }

        close(fa_sniffer);

        /* Pause before retrying.  Ideally should poll sniffer card for
         * active network here. */
        sleep(1);
    }
    return NULL;
}


bool initialise_sniffer(const char * device_name)
{
    fa_sniffer_device = device_name;
    return TEST_0(pthread_create(&sniffer_id, NULL, sniffer_thread, NULL));
}

void terminate_sniffer(void)
{
    log_message("Waiting for sniffer...");
    pthread_cancel(sniffer_id);     // Ignore complaint if already halted
    ASSERT_0(pthread_join(sniffer_id, NULL));
    log_message("done");
}
