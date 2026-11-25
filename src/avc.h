#ifndef CIC_CARLIFE_AVC_H
#define CIC_CARLIFE_AVC_H

#include <stddef.h>
#include <stdint.h>

#define CL_AVC_SPS 1u
#define CL_AVC_PPS 2u
#define CL_AVC_IDR 4u
#define CL_AVC_PICTURE 8u
#define CL_AVC_CONFIG_MAX 508u

int cl_avc_inspect(const uint8_t *, size_t, unsigned *flags,
                   uint8_t config[CL_AVC_CONFIG_MAX], size_t *config_size);

#endif
