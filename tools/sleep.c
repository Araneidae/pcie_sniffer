#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "timing.h"

int main(int argc, char **argv)
{
    for (int i = 0; i < TIMING_BUFFER_SIZE; i ++)
    {
        START_TIMING;
        usleep(1000000);
        STOP_TIMING;
    }
}
