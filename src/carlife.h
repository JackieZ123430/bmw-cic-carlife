#ifndef CIC_CARLIFE_H
#define CIC_CARLIFE_H

#include <stddef.h>
#include <stdint.h>

#define CL_WIDTH 640u
#define CL_HEIGHT 480u
#define CL_FPS 30u
#define CL_MAX_VIDEO 262144u

enum cl_event {
    CL_WAIT_VERSION, CL_WAIT_VIDEO, CL_WAIT_IDR, CL_STREAMING
};

struct cl_io {
    void *context;
    /* USB OUT uses two separate transfers for each message. */
    int (*write)(void *, const uint8_t *, size_t);
    int (*video)(void *, const uint8_t *, size_t, uint32_t timestamp_ms, int has_picture);
    void (*progress)(void *, enum cl_event);
};

struct cl_session;
struct cl_session *cl_open(const struct cl_io *, unsigned major);
void cl_close(struct cl_session *);
int cl_start(struct cl_session *);
int cl_receive(struct cl_session *, const uint8_t *, size_t);
int cl_complete_record(const struct cl_session *);
int cl_request_idr(struct cl_session *);
int cl_pause(struct cl_session *);
int cl_back(struct cl_session *);
const char *cl_error(const struct cl_session *);

#endif
