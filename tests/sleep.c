/* Test of jitter of usleep call. */

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>

#define TIMING_TEST
#include "timing.h"

int main(int argc, char **argv)
{
    for (int i = 0; i < TIMING_BUFFER_SIZE; i ++)
    {
        START_TIMING;
        usleep(10000);
        STOP_TIMING;
    }
}
