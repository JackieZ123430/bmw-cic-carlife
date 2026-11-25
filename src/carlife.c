#include "carlife.h"
#include "avc.h"

#include <stdlib.h>
#include <string.h>

enum {
    CMD = 1, VIDEO = 2, MEDIA = 3, TTS = 4, VR = 5, TOUCH = 6, UPDATE = 7,
    HU_VERSION = 0x18001, MD_VERSION = 0x10002, HU_INFO = 0x18003,
    VIDEO_INIT = 0x18007, VIDEO_READY = 0x10008, VIDEO_START = 0x18009,
    VIDEO_PAUSE = 0x1800a, VIDEO_RESET = 0x1800b,
    MD_FEATURE_REQUEST = 0x10051, HU_FEATURE_RESPONSE = 0x18052,
    VIDEO_DATA = 0x20001, HARD_KEY = 0x68008
};

struct cl_session {
    struct cl_io io;
    const char *error;
    unsigned major, channel, parameters;
    enum cl_event phase;
    int started;
    size_t header_used, body_used, body_size;
    uint8_t header[8];
    uint8_t body[CL_MAX_VIDEO + 12];
};

static int fail(struct cl_session *s, const char *reason)
{
    s->error = reason;
    return -1;
}

static unsigned inner_header(unsigned channel)
{
    return channel == CMD || channel == TOUCH ? 8 : 12;
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}

static void advance(struct cl_session *s, enum cl_event phase)
{
    s->phase = phase;
    if (s->io.progress)
        s->io.progress(s->io.context, phase);
}

static int send_command(struct cl_session *s, uint32_t type, const uint8_t *data, size_t size)
{
    uint8_t outer[8], inner[96];
    if (size > sizeof(inner) - 8)
        return fail(s, "outgoing message exceeds buffer");
    write_be32(outer, type >> 16);
    write_be32(outer + 4, (uint32_t)size + 8);
    write_be32(inner, (uint32_t)size << 16);
    write_be32(inner + 4, type);
    if (size)
        memcpy(inner + 8, data, size);
    if (s->io.write(s->io.context, outer, sizeof outer) ||
        s->io.write(s->io.context, inner, size + 8))
        return fail(s, "CarLife transport write failed");
    return 0;
}

static int read_varint(const uint8_t **cursor, const uint8_t *end, uint64_t *value)
{
    unsigned shift;
    *value = 0;
    for (shift = 0; shift < 64 && *cursor < end; shift += 7) {
        uint8_t byte = *(*cursor)++;
        if (shift == 63 && byte > 1)
            break;
        *value |= (uint64_t)(byte & 127) << shift;
        if (!(byte & 128))
            return 0;
    }
    return -1;
}

/* Only fields 1..3 of VersionMatch and VideoEncoderInitDone affect this port. */
static int read_numbers(const uint8_t *data, size_t length, uint64_t out[3], unsigned required)
{
    const uint8_t *end = data + length;
    unsigned mask = 0;
    while (data != end) {
        uint64_t key, value, field;
        if (read_varint(&data, end, &key) || key < 8 || key > UINT32_MAX)
            return -1;
        field = key >> 3;
        switch (key & 7) {
        case 0:
            if (read_varint(&data, end, &value))
                return -1;
            if (field <= 3) {
                unsigned index = (unsigned)field - 1;
                if (mask & (1u << index))
                    return -1;
                mask |= 1u << index;
                out[index] = value;
            }
            continue;
        case 1:
            value = 8;
            break;
        case 2:
            if (read_varint(&data, end, &value))
                return -1;
            break;
        case 5:
            value = 4;
            break;
        default:
            return -1;
        }
        if (value > (uint64_t)(end - data))
            return -1;
        data += (size_t)value;
    }
    return (mask & required) == required ? 0 : -1;
}

static int on_video(struct cl_session *s, const uint8_t *data, size_t size, uint32_t pts)
{
    uint8_t config[CL_AVC_CONFIG_MAX];
    size_t config_size;
    unsigned flags, next;
    int picture, result;
    if (s->phase < CL_WAIT_IDR)
        return 0; /* Stale media can arrive before the new session is negotiated. */
    if (cl_avc_inspect(data, size, &flags, config, &config_size))
        return fail(s, "H.264 outside CIC candidate mode (640x480 progressive Baseline)");
    picture = (flags & CL_AVC_PICTURE) != 0;
    next = s->parameters | (flags & (CL_AVC_SPS | CL_AVC_PPS));
    if (!picture && !flags)
        return 0;
    if (picture && s->phase == CL_WAIT_IDR &&
        (!(flags & CL_AVC_IDR) || next != (CL_AVC_SPS | CL_AVC_PPS)))
        return 0;
    result = picture ? s->io.video(s->io.context, data, size, pts, 1)
                     : s->io.video(s->io.context, config, config_size, pts, 0);
    if (result < 0)
        return fail(s, "video sink failed");
    if (result > 0)
        return cl_request_idr(s);
    s->parameters = next;
    if (picture && s->phase == CL_WAIT_IDR)
        advance(s, CL_STREAMING);
    return 0;
}

static int on_packet(struct cl_session *s)
{
    static const uint8_t identity[] = {
        0x0a, 3, 'Q','N','X',
        0x22, 11, 'C','I','C',' ','C','a','r','L','i','f','e',
        0x72, 7, 'C','I','C',' ','L','a','b'
    };
    static const uint8_t video_mode[] = {
        0x08, (CL_WIDTH & 127) | 128, CL_WIDTH >> 7,
        0x10, (CL_HEIGHT & 127) | 128, CL_HEIGHT >> 7, 0x18, CL_FPS
    };
    static const uint8_t no_features[] = {0x08, 0x00};
    const unsigned head = inner_header(s->channel);
    const uint8_t *data = s->body + head;
    const uint32_t type = read_be32(data - 4);
    const uint32_t declared = read_be32(s->body);
    const size_t size = s->body_size - head;
    uint64_t values[3] = {0};
    if (type >> 16 != s->channel ||
        (head == 8 ? (declared != ((uint32_t)size << 16)) : (declared != size)))
        return fail(s, "CarLife inner length or channel mismatch");
    switch (type) {
    case MD_VERSION:
        if (s->phase != CL_WAIT_VERSION)
            return fail(s, "unexpected CarLife VERSION_MATCH");
        if (read_numbers(data, size, values, 1) || values[0] != 1)
            return fail(s, "phone rejected CarLife protocol version");
        if (send_command(s, HU_INFO, identity, sizeof identity) ||
            send_command(s, VIDEO_INIT, video_mode, sizeof video_mode))
            return -1;
        advance(s, CL_WAIT_VIDEO);
        return 0;
    case MD_FEATURE_REQUEST:
        if (s->phase < CL_WAIT_VIDEO)
            return fail(s, "feature request before version match");
        return send_command(s, HU_FEATURE_RESPONSE, no_features, sizeof no_features);
    case VIDEO_READY:
        if (s->phase != CL_WAIT_VIDEO)
            return fail(s, "unexpected CarLife VIDEO_INIT_DONE");
        if (read_numbers(data, size, values, 7) ||
            values[0] != CL_WIDTH || values[1] != CL_HEIGHT || !values[2] || values[2] > CL_FPS)
            return fail(s, "phone did not accept the CIC 640x480 video mode");
        if (send_command(s, VIDEO_START, NULL, 0))
            return -1;
        advance(s, CL_WAIT_IDR);
        return 0;
    case VIDEO_DATA:
        return on_video(s, data, size, read_be32(s->body + 4));
    case 0x10049: /* phone verification response */
    case 0x1004b: /* phone's independent HU authorization result */
    case 0x1006a: /* activation */
    case 0x1006c: /* content key exchange */
    case 0x1006e: /* encryption ready */
        return fail(s, "CarLife authorization/activation/encryption branch requires a supported profile");
    default:
        return 0;
    }
}

struct cl_session *cl_open(const struct cl_io *io, unsigned major)
{
    struct cl_session *s;
    if (!io || !io->write || !io->video || (major != 1 && major != 2))
        return NULL;
    s = calloc(1, sizeof *s);
    if (s) {
        s->io = *io;
        s->major = major;
    }
    return s;
}

void cl_close(struct cl_session *s)
{
    free(s);
}

int cl_start(struct cl_session *s)
{
    uint8_t version[] = {0x08, (uint8_t)s->major, 0x10, 0x00};
    if (s->started || s->error)
        return -1;
    s->started = 1;
    advance(s, CL_WAIT_VERSION);
    return send_command(s, HU_VERSION, version, sizeof version);
}

int cl_receive(struct cl_session *s, const uint8_t *data, size_t size)
{
    if (!s->started || s->error)
        return -1;
    while (size) {
        size_t count;
        if (s->header_used < sizeof s->header) {
            count = sizeof s->header - s->header_used;
            if (count > size)
                count = size;
            memcpy(s->header + s->header_used, data, count);
            s->header_used += count;
            data += count;
            size -= count;
            if (s->header_used != sizeof s->header)
                continue;
            s->channel = read_be32(s->header);
            s->body_size = read_be32(s->header + 4);
            if (s->channel < CMD || s->channel > UPDATE ||
                s->body_size < inner_header(s->channel) || s->body_size > sizeof s->body ||
                (inner_header(s->channel) == 8 && s->body_size > 65535u + 8u))
                return fail(s, "invalid CarLife AOA record header");
        }
        count = s->body_size - s->body_used;
        if (count > size)
            count = size;
        memcpy(s->body + s->body_used, data, count);
        s->body_used += count;
        data += count;
        size -= count;
        if (s->body_used == s->body_size) {
            if (on_packet(s))
                return -1;
            s->body_used = s->header_used = 0;
        }
    }
    return 0;
}

int cl_complete_record(const struct cl_session *s)
{
    return s->header_used == 0;
}

int cl_request_idr(struct cl_session *s)
{
    if (s->error || s->phase < CL_WAIT_IDR)
        return 0;
    if (s->major == 1) {
        /* A reset can renegotiate video in v1. Reopen the session on the bench. */
        advance(s, CL_WAIT_IDR);
        return 0;
    }
    advance(s, CL_WAIT_IDR);
    return send_command(s, VIDEO_RESET, NULL, 0);
}

int cl_pause(struct cl_session *s)
{
    if (s->error || s->phase < CL_WAIT_IDR)
        return 0;
    return send_command(s, VIDEO_PAUSE, NULL, 0);
}

int cl_back(struct cl_session *s)
{
    static const uint8_t back[] = {0x08, 14};
    if (s->error || s->phase != CL_STREAMING)
        return 0;
    return send_command(s, HARD_KEY, back, sizeof back);
}

const char *cl_error(const struct cl_session *s)
{
    return s->error ? s->error : "no complete CarLife video stream";
}
