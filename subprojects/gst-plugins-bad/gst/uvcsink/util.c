#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/time.h>

#include "util.h"

size_t hexdump(
    char *dst, const size_t capacity,
    const void *src, size_t size)
{
    const char lookup[] =
    {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
    };

    const uint8_t *src_begin = src;
    const uint8_t *const src_end = src_begin + size;
    char *begin = dst;
    const char *const end = begin + capacity;

    while(src_begin != src_end && begin != end)
    {
        *begin++ = lookup[*src_begin >> 4];
        *begin++ = lookup[*src_begin & UINT8_C(0x0F)];
        ++src_begin;
    }
    return (src_begin - (const uint8_t *)src) << 1;
}

int64_t timestamp_us(void)
{
    struct timeval tval;

    gettimeofday(&tval, NULL);
    return (int64_t)tval.tv_sec * 1000000 + (int64_t)tval.tv_usec;
}

int64_t timestamp_ms(void)
{
    return timestamp_us() / INT64_C(1000);
}

int64_t timespec_to_us(struct timespec value)
{
    return (int64_t)value.tv_sec * 1000000 + (int64_t)value.tv_nsec / 1000;
}

int64_t timespec_to_ms(struct timespec value)
{
    return timespec_to_us(value) / INT64_C(1000);
}

void dump_to_file(const char *name, const void *data, size_t size)
{
    assert(name);
    assert(data);
    assert(size);
    if(!name || !data || 0u == size) return;

    FILE *file = fopen(name, "w");

    assert(file);
    if(!file) return;

    if(file)
    {
        size_t written = fwrite(data, 1, size, file);
        assert(written == size);
        fclose(file);
        file = NULL;
    }
}
