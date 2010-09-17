/* Simple timing support. */

#define TIMING_BUFFER_SIZE  128

static uint64_t __timing_buffer[TIMING_BUFFER_SIZE];
static int __timing_count = 0;
static bool __timing_reported = false;


static __inline__ uint64_t get_ticks(void)
{
    uint64_t ticks;
    __asm__ __volatile__("rdtsc" : "=A"(ticks));
    return ticks;
}

static void update_timing(uint64_t interval)
{
    if (__timing_count < TIMING_BUFFER_SIZE)
        __timing_buffer[__timing_count++] = interval;
    if (__timing_count == TIMING_BUFFER_SIZE  &&  !__timing_reported)
    {
        double sum = 0;
        for (int i = 0; i < TIMING_BUFFER_SIZE; i ++)
        {
            uint64_t t = __timing_buffer[i];
            printf("%"PRIu64" ", t);
            if (i % 8 == 7)
                printf("\n");
            sum += t;
        }
        printf("mean: %g\n", sum / TIMING_BUFFER_SIZE);
        __timing_reported = true;
    }
}

#define START_TIMING    uint64_t __timing_start__ = get_ticks()
#define STOP_TIMING  \
    uint64_t __timing_stop__ = get_ticks(); \
    update_timing(__timing_stop__ - __timing_start__)
