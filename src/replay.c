#include "carlife.h"

#include <stdio.h>
#include <stdlib.h>

struct replay {
    FILE *video;
    FILE *tx;
    unsigned frames;
};

static int write_record(void *context, const uint8_t *data, size_t length)
{
    struct replay *r = context;
    return fwrite(data, 1, length, r->tx) == length ? 0 : -1;
}

static int write_video(void *context, const uint8_t *data, size_t length,
                       uint32_t timestamp_ms, int has_picture)
{
    struct replay *r = context;
    (void)timestamp_ms;
    r->frames += has_picture != 0;
    return fwrite(data, 1, length, r->video) == length ? 0 : -1;
}

int main(int argc, char **argv)
{
    struct replay r = {0};
    struct cl_io io = {&r, write_record, write_video, NULL};
    struct cl_session *session = NULL;
    FILE *input = NULL;
    uint8_t buffer[16384];
    size_t count, chunk = sizeof buffer;
    unsigned long major;
    char *end;
    int code = 1;

    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: replay <1|2> <incoming.aoa> <video.h264> <outgoing.aoa> [read-size]\n");
        return 2;
    }
    major = strtoul(argv[1], &end, 10);
    if (*end || (major != 1 && major != 2))
        return 2;
    if (argc == 6) {
        unsigned long value = strtoul(argv[5], &end, 10);
        if (*end || !value || value > sizeof buffer)
            return 2;
        chunk = (size_t)value;
    }

    input = fopen(argv[2], "rb");
    if (!input) {
        perror(argv[2]);
        goto done;
    }
    r.video = fopen(argv[3], "wbx");
    if (!r.video) {
        perror(argv[3]);
        goto done;
    }
    r.tx = fopen(argv[4], "wbx");
    if (!r.tx) {
        perror(argv[4]);
        goto done;
    }
    session = cl_open(&io, (unsigned)major);
    if (!session) {
        fputs("Could not allocate CarLife session\n", stderr);
        goto done;
    }
    if (cl_start(session))
        goto protocol_error;
    while ((count = fread(buffer, 1, chunk, input)) != 0)
        if (cl_receive(session, buffer, count))
            goto protocol_error;
    if (ferror(input)) {
        perror(argv[2]);
        goto done;
    }
    if (!cl_complete_record(session)) {
        fputs("Truncated CarLife AOA record\n", stderr);
        goto done;
    }
    if (!r.frames)
        goto protocol_error;
    code = 0;
    goto done;

protocol_error:
    fprintf(stderr, "%s\n", cl_error(session));

done:
    cl_close(session);
    if (input)
        fclose(input);
    if (r.video && fclose(r.video))
        code = 1;
    if (r.tx && fclose(r.tx))
        code = 1;
    if (!code)
        printf("CarLife H.264 access units: %u (%ux%u)\n", r.frames, CL_WIDTH, CL_HEIGHT);
    return code;
}
