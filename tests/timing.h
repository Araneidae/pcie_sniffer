/* Simple timing support.
 *
 * Copyright (c) 2011 Michael Abbott, Diamond Light Source Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Contact:
 *      Dr. Michael Abbott,
 *      Diamond Light Source Ltd,
 *      Diamond House,
 *      Chilton,
 *      Didcot,
 *      Oxfordshire,
 *      OX11 0DE
 *      michael.abbott@diamond.ac.uk
 */


static __inline__ uint64_t get_ticks(void)
{
#if defined(__i386__)
    uint64_t ticks;
    __asm__ __volatile__("rdtsc" : "=A"(ticks));
    return ticks;
#elif defined(__x86_64__)
    uint32_t high, low;
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t) high << 32) | low;
#endif
}


#ifdef TIMING_TEST

#define TIMING_BUFFER_SIZE  128

static uint64_t __timing_buffer[TIMING_BUFFER_SIZE];
static int __timing_count = 0;
static bool __timing_reported = false;

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
            sum += (double) t;
        }
        double mean = sum / TIMING_BUFFER_SIZE;
        double variance = 0;
        for (int i = 0; i < TIMING_BUFFER_SIZE; i ++)
        {
            double t = (double) __timing_buffer[i];
            variance += (t - mean) * (t - mean);
        }
        variance /= TIMING_BUFFER_SIZE;
        printf("mean: %g, std: %g\n", mean, sqrt(variance));
        __timing_reported = true;
    }
}

#define START_TIMING    uint64_t __timing_start__ = get_ticks()
#define STOP_TIMING  \
    uint64_t __timing_stop__ = get_ticks(); \
    update_timing(__timing_stop__ - __timing_start__)
#endif
