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


int main(int argc, char **argv)
{
    char *filename = argv[1];
    int64_t s = atoll(argv[2]);
    int l = atol(argv[3]);

    char *buffer;
    int file_fd;
    bool ok =
        TEST_NULL(buffer = valloc(l))  &&
        TEST_IO(file_fd = open(filename, O_RDONLY | O_DIRECT))  &&
        TEST_IO(lseek(file_fd, s, SEEK_SET))  &&
        TEST_IO(read(file_fd, buffer, l))  &&
        TEST_OK(fwrite(buffer, l, 1, stdout) == 1);
    return ok ? 0 : 2;
}
