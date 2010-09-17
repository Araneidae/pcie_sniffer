/* Support for writing a matlab header on capture FA data. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "sniffer.h"
#include "mask.h"
#include "error.h"

#include "matlab.h"


/* The matlab format symbol definitions we use. */
#define miINT8          1
#define miUINT8         2
#define miINT32         5
#define miUINT32        6
#define miMATRIX        14

#define mxDOUBLE_CLASS  6
#define mxINT32_CLASS   12


static void compute_mask_ids(uint8_t *array, filter_mask_t mask)
{
    for (int bit = 0; bit < 256; bit ++)
        if (test_mask_bit(mask, bit))
            *array++ = bit;
}


static void write_matlab_string(int32_t **hh, const char *string)
{
    int32_t *h = *hh;
    int l = strlen(string);
    *h++ = miINT8;      *h++ = l;
    memcpy(h, string, l);
    *hh = h + 2 * ((l + 7) / 8);
}


/* Returns the number of bytes of padding required after data_length bytes of
 * following data to ensure that the entire matrix is padded to 8 bytes. */
static int write_matrix_header(
    int32_t **hh, int class, const char *name,
    int data_type, bool squeeze, int data_length,
    int dimensions, ...)
{
    va_list dims;
    va_start(dims, dimensions);

    int32_t *h = *hh;
    *h++ = miMATRIX;
    int32_t *l = h++;   // total length will be written here.
    // Matrix flags: consists of two uint32 words encoding the class.
    *h++ = miUINT32;    *h++ = 8;
    *h++ = class;
    *h++ = 0;

    // Matrix dimensions: one int32 for each dimension
    *h++ = miINT32;
    int32_t *dim_size = h++;    // Size of dimensions to be written here
    int squeezed_dims = 0;
    for (int i = 0; i < dimensions; i ++)
    {
        int size = va_arg(dims, int32_t);
        if (size != 1  ||  !squeeze)
        {
            *h++ = size;
            squeezed_dims += 1;
        }
    }
    *dim_size = squeezed_dims * sizeof(int32_t);
    h += squeezed_dims & 1;    // Padding if required

    // Element name
    write_matlab_string(&h, name);

    // Data header: data follows directly after.
    int padding = (8 - data_length) & 7;
    *h++ = data_type;   *h++ = data_length;
    *l = data_length + (h - l - 1) * sizeof(int32_t) + padding;

    *hh = h;
    return padding;
}


/* Advances pointer by length together with the precomputed padding. */
static void pad(int32_t **hh, int length, int padding)
{
    *hh = (int32_t *)((char *)*hh + length + padding);
}


bool write_matlab_header(
    int file_out, filter_mask_t filter_mask, unsigned int data_mask,
    unsigned int decimation,
    unsigned int dump_length, const char *name, bool squeeze)
{
    char mat_header[4096];
    memset(mat_header, 0, sizeof(mat_header));

    /* The first 128 bytes are the description and format marks. */
    memset(mat_header, ' ', 124);
    sprintf(mat_header, "MATLAB 5.0 MAT-file generated from FA sniffer data");
    mat_header[strlen(mat_header)] = ' ';
    *(uint16_t *)&mat_header[124] = 0x0100;   // Version flag
    *(uint16_t *)&mat_header[126] = 0x4d49;   // 'IM' endian mark
    int32_t *h = (int32_t *)&mat_header[128];

    int mask_length = count_mask_bits(filter_mask);

    /* Write out the decimation as a matrix(!) */
    int padding = write_matrix_header(&h,
        mxINT32_CLASS, "decimation", miINT32, false, sizeof(int32_t), 1, 1);
    *h++ = decimation;
    pad(&h, 0, padding);

    /* Write out the index array tying data back to original BPM ids. */
    padding = write_matrix_header(&h,
        mxDOUBLE_CLASS, "ids", miUINT8, false, mask_length, 2, 1, mask_length);
    compute_mask_ids((uint8_t *)h, filter_mask);
    pad(&h, mask_length, padding);

    /* Finally write out the matrix mat_header for the fa data. */
    int field_count = count_data_bits(data_mask);
    write_matrix_header(&h,
        mxDOUBLE_CLASS, name, miINT32, squeeze,
        dump_length * field_count * mask_length * FA_ENTRY_SIZE,
        4, 2, field_count, mask_length, dump_length);

    return TEST_write(file_out, mat_header, (char *) h - mat_header);
}


unsigned int count_data_bits(unsigned int mask)
{
    return
        ((mask >> 0) & 1) + ((mask >> 1) & 1) +
        ((mask >> 2) & 1) + ((mask >> 3) & 1);
}
