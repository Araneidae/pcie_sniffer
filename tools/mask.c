/* Filter mask routines. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

#include "mask.h"



int count_mask_bits(filter_mask_t mask)
{
    int count = 0;
    for (int bit = 0; bit < 256; bit ++)
        if (test_mask_bit(mask, bit))
            count ++;
    return count;
}


int format_mask(filter_mask_t mask, char *buffer)
{
    for (int i = sizeof(filter_mask_t) / 4; i > 0; i --)
        buffer += sprintf(buffer, "%08x", mask[i - 1]);
    return (sizeof(filter_mask_t) / 4) * 8;
}

void print_mask(FILE *out, filter_mask_t mask)
{
    char buffer[2 * sizeof(filter_mask_t) + 1];
    fwrite(buffer, format_mask(mask, buffer), 1, out);
}



static bool read_id(char *original, char **string, int *id)
{
    char *next;
    *id = strtol(*string, &next, 0);
    return
        TEST_OK_(next > *string,
            "Number missing at \"%s\" (+%d)", original, *string - original)  &&
        DO_(*string = next)  &&
        TEST_OK_(0 <= *id  &&  *id < 256, "id %d out of range", *id);
}

bool parse_mask(char *string, filter_mask_t mask)
{
    char *original = string;    // Just for error reporting
    memset(mask, 0, sizeof(filter_mask_t));
    
    bool ok = true;
    while (ok)
    {
        int id;
        ok = read_id(original, &string, &id);
        if (ok)
        {
            if (*string == '-')
            {
                string ++;
                int end;
                ok = read_id(original, &string, &end);
                for (int i = id; i <= end  &&  ok; i ++)
                    set_mask_bit(mask, i);
            }
            else
                set_mask_bit(mask, id);
        }
        
        if (*string != ',')
            break;
        string ++;
    }

    return
        ok  &&
        TEST_OK_(*string == '\0',
            "Unexpected characters at \"%s\" (+%d)",
            original, string - original);
}


int copy_frame(void *to, void *from, filter_mask_t mask)
{
    int32_t *from_p = from;
    int32_t *to_p = to;
    int copied = 0;
    for (size_t i = 0; i < sizeof(filter_mask_t) / 4; i ++)
    {
        uint32_t m = mask[i];
        for (int j = 0; j < 32; j ++)
        {
            if ((m >> j) & 1)
            {
                *to_p++ = from_p[0];
                *to_p++ = from_p[1];
                copied += 8;
            }
            from_p += 2;
        }
    }
    return copied;
}
