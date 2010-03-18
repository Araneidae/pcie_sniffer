/* Utility to extract archived data from an archiver file. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "error.h"
#include "disk.h"
#include "mask.h"


#define USAGE_STRING \
    "Usage: read [options] <archive-file>\n" \
    "Reads stuff ...\n" \
    "Options:\n" \
    "   -m: Select BPM ids to be read from archive\n" \
    "   -H  Show header information\n" \
    "   -h  Shows this help text\n" 



filter_mask_t filter_mask;

char * file_name;
int file_fd;

uint64_t file_size;
struct disk_header header;


FILE * out_file;
bool show_header = false;
int dump_length = 0;
int64_t dump_start;


bool parse_int(char *string, int *result)
{
    char *end;
    *result = strtol(string, &end, 0);
    return
        TEST_OK_(end > string,
            "No number in argument \"%s\"", string)   &&
        TEST_OK_(*end == '\0',
            "Unexpected characters after number \"%s\"", string);
}


bool parse_int64(char *string, int64_t *result)
{
    char *end;
    *result = strtoll(string, &end, 0);
    return
        TEST_OK_(end > string,
            "No number in argument \"%s\"", string)   &&
        TEST_OK_(*end == '\0',
            "Unexpected characters after number \"%s\"", string);
}


bool parse_opts(int *argc, char ***argv)
{
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hm:o:Hl:s:"))
        {
            case 'h':
                printf(USAGE_STRING);
                exit(0);    // Special case: no error, yet no processing!
            case 'm':
                ok = parse_mask(optarg, filter_mask);
                break;
            case 'o':
                ok = TEST_NULL_(
                    out_file = fopen(optarg, "w"),
                    "Unable to open output file \"%s\"", optarg);
                break;
            case 'H':
                show_header = true;
                break;
            case 'l':
                ok = parse_int(optarg, &dump_length);
                break;
            case 's':
                ok = parse_int64(optarg, &dump_start);
                break;

            case '?':
            default:
                fprintf(stderr, "Try `%s -h` for usage\n", (*argv)[0]);
                return false;
            case -1:
                *argc -= optind;
                *argv += optind;
                return true;
        }
    }
    return ok;
}


bool parse_args(int argc, char **argv)
{
    if (argc == 1)
    {
        file_name = argv[0];
        return true;
    }
    else
    {
        fprintf(stderr, "Try -h for usage\n");
        return false;
    }
}



void dump_data(FILE *out_file)
{
    off64_t start = header.blocks[0].stop_offset - dump_length * 2048;
    if (!TEST_OK_(start >= 0, "Don't do wrapping yet!"))
        return;
    
    ASSERT_IO(lseek(file_fd, header.h.data_start + start, SEEK_SET));
    char read_buffer[2048];
    char write_buffer[2048];
    for (int i = 0; i < dump_length; i ++)
    {
        ASSERT_OK(read(file_fd, read_buffer, 2048) == 2048);
        size_t n = copy_frame(write_buffer, read_buffer, filter_mask);
        ASSERT_OK(fwrite(write_buffer, n, 1, out_file) == 1);
    }
}



int main(int argc, char **argv)
{
    memset(filter_mask, 0xff, sizeof(filter_mask_t));
    out_file = stdout;
    
    if (parse_opts(&argc, &argv)  &&  parse_args(argc, argv))
    {
        bool ok =
            TEST_IO_(
                file_fd = open(file_name, O_RDONLY),
                "Unable to open file \"%s\"", file_name)  &&
            get_filesize(file_fd, &file_size)  &&
            read_header(file_fd, &header)  &&
            validate_header(&header, file_size);

        if (ok)
        {
//            print_mask(stdout, filter_mask); printf("\n");
            if (show_header)
                print_header(out_file, &header);
            if (dump_length > 0)
                dump_data(out_file);
        }
        
        return ok ? 0 : 2;
    }
    else
        return 1;
}
