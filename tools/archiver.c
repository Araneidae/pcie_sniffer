/* Archiver program for capturing data from FA sniffer and writing to disk.
 * Also makes the continuous data stream available over a dedicated socket. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "error.h"
#include "buffer.h"
#include "sniffer.h"
#include "disk_writer.h"


#define K               1024
#define FA_BLOCK_SIZE   (64 * K)    // Block size for device read


static bool daemon_mode = false;


/*****************************************************************************/
/*                               Option Parsing                              */
/*****************************************************************************/

static char *argv0;
static char *fa_sniffer_device = "/dev/fa_sniffer0";
static char *output_filename = NULL;
static char *pid_filename = NULL;
/* A good default buffer size is 8K blocks, or 256MB. */
static unsigned int buffer_size = 8 * K * FA_BLOCK_SIZE;



static void usage(void)
{
    printf(
"Usage: %s [options] <archive-file>\n"
"Captures continuous FA streaming data to disk\n"
"\n"
"Options:\n"
"    -d:  Specify device to use for FA sniffer (default /dev/fa_sniffer0)\n"
"    -b:  Specify buffer size (default 128M bytes)\n"
"    -v   Specify verbose output\n"
"    -D   Run as a daemon\n"
"    -p:  Write PID to specified file\n"
        , argv0);
}

static bool read_size(char *string, unsigned int *result)
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


static bool process_options(int *argc, char ***argv)
{
    argv0 = (*argv)[0];
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hd:b:vDp:"))
        {
            case 'h':   usage();                                    exit(0);
            case 'd':   fa_sniffer_device = optarg;                 break;
            case 'b':   ok = read_size(optarg, &buffer_size);       break;
            case 'v':   verbose_logging(true);                      break;
            case 'D':   daemon_mode = true;                         break;
            case 'p':   pid_filename = optarg;                      break;
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

static bool process_args(int argc, char **argv)
{
    return
        process_options(&argc, &argv)  &&
        TEST_OK_(argc == 1, "Try `%s -h` for usage", argv0)  &&
        DO_(output_filename = argv[0]);
}



/*****************************************************************************/
/*                            Startup and Control                            */
/*****************************************************************************/


static sem_t shutdown_semaphore;


static void at_exit(int signal)
{
    log_message("Signal caught");
    if (pid_filename)
        TEST_IO(unlink(pid_filename));
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


static bool maybe_daemonise()
{
    int pid_file = -1;
    char pid[32];
    return
        /* The logic here is a little odd: we want to check that we can write
         * the PID file before daemonising, to ensure that the caller gets the
         * error message if daemonising fails, but we need to write the PID file
         * afterwards to get the right PID. */
        IF_(pid_filename,
            TEST_IO_(pid_file = open(
                pid_filename, O_WRONLY | O_CREAT | O_EXCL, 0644),
                "PID file already exists: is archiver already running?"))  &&
        IF_(daemon_mode,
            /* Don't chdir to / so that we can rmdir(pid_filename) at end. */
            TEST_IO(daemon(true, false))  &&
            DO_(start_logging("FA archiver")))  &&
        IF_(pid_filename,
            DO_(sprintf(pid, "%d", getpid()))  &&
            TEST_IO(write(pid_file, pid, strlen(pid)))  &&
            TEST_IO(close(pid_file)));
}


int main(int argc, char **argv)
{
    bool ok =
        process_args(argc, argv)  &&
        initialise_signals()  &&
        maybe_daemonise()  &&
        /* All the thread initialisation must be done after daemonising, as of
         * course threads don't survive across the daemon() call!  Alas, this
         * means that many startup errors go into syslog rather than stderr. */
        initialise_buffer(FA_BLOCK_SIZE, buffer_size / FA_BLOCK_SIZE)  &&
        initialise_disk_writer(output_filename, buffer_size)  &&
        initialise_sniffer(fa_sniffer_device);

    if (ok)
    {
        log_message("Started");

        /* Wait for a shutdown signal.  We actually ignore the signal, instead
         * waiting for the clean shutdown request. */
        while (sem_wait(&shutdown_semaphore) == -1  &&  TEST_OK(errno == EINTR))
            ; /* Repeat wait while we see errno. */

        log_message("Shutting down");
        terminate_sniffer();
        terminate_disk_writer();
        log_message("Shut Down");
    }
}
