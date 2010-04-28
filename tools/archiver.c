/* Archiver program for capturing data from FA sniffer and writing to disk.
 * Also makes the continuous data stream available over a dedicated socket. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <semaphore.h>
#include <signal.h>

#include "error.h"
#include "buffer.h"
#include "sniffer.h"
#include "disk_writer.h"


#define K               1024
#define FA_BLOCK_SIZE   (64 * K)    // Block size for device read 



/*****************************************************************************/
/*                               Option Parsing                              */
/*****************************************************************************/

static char *argv0;
static char *fa_sniffer_device = "/dev/fa_sniffer0";
static char *output_file = NULL;
/* A good default buffer size is 8K blocks, or 256MB. */
static unsigned int buffer_size = 8 * K * FA_BLOCK_SIZE;

static bool verbose = false;



void usage(void)
{
    printf(
"Usage: %s [options] <archive-file>\n"
"Captures continuous FA streaming data to disk\n"
"\n"
"Options:\n"
"    -d:  Specify device to use for FA sniffer (default /dev/fa_sniffer0)\n"
"    -b:  Specify buffer size (default 128M bytes)\n"
"    -v   Specify verbose output\n"
        , argv0);
}

bool read_size(char *string, unsigned int *result)
{
    char *end;
    *result = strtoul(string, &end, 0);
    if (TEST_OK_(end > string, "nothing specified for option")) {
        switch (*end) {
            case 'K':   end++;  *result *= K;           break;
            case 'M':   end++;  *result *= K * K;       break;
            case '\0':  break;
        }
        return TEST_OK_(*end == '\0',
            "Unexpected characters in integer \"%s\"", string);
    } else
        return false;
}


bool process_options(int *argc, char ***argv)
{
    argv0 = (*argv)[0];
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hd:b:v"))
        {
            case 'h':   usage();                                    exit(0);
            case 'd':   fa_sniffer_device = optarg;                 break;
            case 'b':   ok = read_size(optarg, &buffer_size);       break;
            case 'v':   verbose = true;                             break;
            default:
                fprintf(stderr, "Try `%s -h` for usage\n", argv0);
                return false;
            case -1:
                *argc -= optind;
                *argv += optind;
                return true;
        }
    }
    return false;
}

bool process_args(int argc, char **argv)
{
    return
        TEST_OK_(argc == 1, "Try `%s -h` for usage\n", argv0)  &&
        DO_(output_file = argv[0]);
}



/*****************************************************************************/
/*                            Startup and Control                            */
/*****************************************************************************/


static sem_t shutdown_semaphore;


static void at_exit(int signal)
{
    printf("at_exit called\n");
    close(STDIN_FILENO);
    ASSERT_IO(sem_post(&shutdown_semaphore));
}

static bool initialise_signals(void)
{
    struct sigaction action;
    action.sa_handler = at_exit;
    action.sa_flags = 0;
    return
        TEST_IO(sem_init(&shutdown_semaphore, 0, 0))  &&
        TEST_IO(sigfillset(&action.sa_mask))  &&
        TEST_IO(sigaction(SIGHUP,  &action, NULL))  &&
        TEST_IO(sigaction(SIGINT,  &action, NULL))  &&
        TEST_IO(sigaction(SIGQUIT, &action, NULL))  &&
        TEST_IO(sigaction(SIGTERM, &action, NULL));
}


void process_command(char *line)
{
    printf("Reading command: \"%s\"\n", line);
    at_exit(0);
}


int main(int argc, char **argv)
{
    bool ok =
        process_options(&argc, &argv)  &&
        process_args(argc, argv)  &&
        initialise_buffer(FA_BLOCK_SIZE, buffer_size / FA_BLOCK_SIZE)  &&
        initialise_sniffer(fa_sniffer_device)  &&
        initialise_disk_writer(output_file, buffer_size)  &&
        initialise_signals();

    if (ok)
    {
        printf("running\n");
        char line[80];
        while (
                printf("> "),
                fflush(stdout),
                fgets(line, sizeof(line), stdin))
            process_command(line);

        terminate_sniffer();
        terminate_disk_writer();
        printf("done\n");
    }
}
