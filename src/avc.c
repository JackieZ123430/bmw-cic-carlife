#include "avc.h"
#include "carlife.h"

#include <string.h>

/* 11 bytes of avcC overhead plus both parameter sets must fit in 512 bytes. */
#define MAX_PARAMETER_SET 250u

struct bits {
    uint8_t data[MAX_PARAMETER_SET];
    size_t length;
    size_t offset;
    int error;
};

static unsigned bit(struct bits *b)
{
    size_t offset = b->offset++;
    if (offset / 8 >= b->length) {
        b->error = 1;
        return 0;
    }
    return (b->data[offset / 8] >> (7 - offset % 8)) & 1u;
}

static unsigned ue(struct bits *b)
{
    unsigned zeros = 0, value = 1;
    while (!bit(b)) {
        if (++zeros == 16 || b->error) {
            b->error = 1;
            return 0;
        }
    }
    while (zeros--)
        value = value * 2 + bit(b);
    return value - 1;
}

static int valid_sps(const uint8_t *nal, size_t length)
{
    struct bits b;
    size_t i;
    unsigned zeros = 0, poc_type, width, height;
    if (length < 5 || length > MAX_PARAMETER_SET || nal[1] != 66 || nal[3] > 31)
        return 0;

    b.length = b.offset = 0;
    b.error = 0;
    for (i = 4; i < length; ++i) {
        if (zeros == 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        b.data[b.length++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    if (ue(&b) > 31 || ue(&b) > 12)
        return 0;
    poc_type = ue(&b);
    if (poc_type == 0 && ue(&b) > 12)
        return 0;
    /* Scope the candidate to Baseline progressive clips with simple POC. */
    if (poc_type == 1 || poc_type > 2 || ue(&b) > 4)
        return 0;
    (void)bit(&b);
    width = (ue(&b) + 1) * 16;
    height = (ue(&b) + 1) * 16;
    if (!bit(&b))
        return 0;
    (void)bit(&b);
    if (bit(&b))
        return 0;
    return !b.error && width == CL_WIDTH && height == CL_HEIGHT;
}

static size_t start_code(const uint8_t *data, size_t size, size_t from, size_t *length)
{
    for (; from + 2 < size; ++from) {
        if (data[from] || data[from + 1])
            continue;
        if (data[from + 2] == 1) {
            *length = 3;
            return from;
        }
        if (from + 3 < size && !data[from + 2] && data[from + 3] == 1) {
            *length = 4;
            return from;
        }
    }
    *length = 0;
    return size;
}

int cl_avc_inspect(const uint8_t *data, size_t size, unsigned *flags,
                   uint8_t config[CL_AVC_CONFIG_MAX], size_t *config_size)
{
    static const uint8_t prefix4[] = {0, 0, 0, 1};
    size_t pos, prefix, i;
    unsigned found = 0;
    *config_size = 0;
    pos = start_code(data, size, 0, &prefix);
    if (pos == size)
        return -1;
    for (i = 0; i < pos; ++i)
        if (data[i])
            return -1;
    while (pos < size) {
        size_t begin = pos + prefix;
        size_t end = start_code(data, size, begin, &prefix);
        size_t length = end - begin;
        unsigned type;
        while (length && !data[begin + length - 1])
            --length;
        if (!length || (data[begin] & 128u))
            return -1;
        type = data[begin] & 31u;
        switch (type) {
        case 7:
            if ((found & CL_AVC_SPS) || !valid_sps(data + begin, length))
                return -1;
            found |= CL_AVC_SPS;
            break;
        case 8:
            if ((found & CL_AVC_PPS) || length < 2 || length > MAX_PARAMETER_SET)
                return -1;
            found |= CL_AVC_PPS;
            break;
        case 5:
            found |= CL_AVC_IDR | CL_AVC_PICTURE;
            break;
        case 1:
            found |= CL_AVC_PICTURE;
            break;
        case 6: /* SEI */
        case 9: /* access unit delimiter */
        case 12: /* filler */
            break;
        default:
            return -1;
        }
        if (type == 7 || type == 8) {
            memcpy(config + *config_size, prefix4, sizeof prefix4);
            *config_size += sizeof prefix4;
            memcpy(config + *config_size, data + begin, length);
            *config_size += length;
        }
        pos = end;
    }
    *flags = found;
    return 0;
}
