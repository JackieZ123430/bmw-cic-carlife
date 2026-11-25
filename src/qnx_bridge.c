/* Local adapter for the pinned AAPlugin ABI. The upstream SDK/driver and
 * video adapter are supplied separately to the build. */
#include "carlife.h"
#include "aa_usb.h"
#include "aa_video.h"
#include "aap_paths.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int busy, disconnect_requested, back_requested, idr_requested;
static const char *status_text = "CarLife idle";
static const char *result_text = "";

struct bridge {
    aa_transport_t usb;
    uint8_t input[16384];
    uint8_t picture[CL_MAX_VIDEO + 8];
    enum cl_event phase;
    uint64_t phase_at;
    int phase_known;
};

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void set_status(const char *text)
{
    pthread_mutex_lock(&lock);
    status_text = text;
    pthread_mutex_unlock(&lock);
}

static void quiet_log(const char *fmt, ...)
{
    (void)fmt;
}

static void usb_status(const char *text)
{
    (void)text; /* the upstream callback can point at temporary stack memory */
    set_status("CarLife: opening Android USB accessory");
}

static void update_progress(void *context, enum cl_event phase)
{
    static const char *const stages[] = {
        "CarLife: waiting for protocol version", "CarLife: waiting for 640x480",
        "CarLife: waiting for H.264 IDR", "CarLife: video active"
    };
    struct bridge *b = context;
    if (!b->phase_known || b->phase != phase)
        b->phase_at = now_ms();
    b->phase_known = 1;
    b->phase = phase;
    set_status(stages[phase]);
}

static int write_usb(void *context, const uint8_t *data, size_t size)
{
    struct bridge *b = context;
    int n = (int)size;
    return b->usb.send(b->usb.ctx, data, n) == n ? 0 : -1;
}

static int push_video(void *context, const uint8_t *data, size_t size,
                      uint32_t timestamp_ms, int has_picture)
{
    struct bridge *b = context;
    unsigned i;
    uint64_t timestamp;
    if (size > CL_MAX_VIDEO - 8)
        return -1;
    if (!has_picture)
        return av_push(1, data, size) ? 1 : 0;
    timestamp = (uint64_t)timestamp_ms * 1000;
    for (i = 0; i < 8; ++i)
        b->picture[7 - i] = (uint8_t)(timestamp >> (8 * i));
    memcpy(b->picture + 8, data, size);
    return av_push(0, b->picture, size + 8) ? 1 : 0;
}

static int take_actions(int *back, int *idr)
{
    int disconnect;
    pthread_mutex_lock(&lock);
    disconnect = disconnect_requested;
    *back = back_requested;
    *idr = idr_requested;
    back_requested = idr_requested = 0;
    pthread_mutex_unlock(&lock);
    return disconnect;
}

static void *worker(void *unused)
{
    static struct bridge b; /* same lifetime as the HMI module, one worker at a time */
    struct cl_io io = {&b, write_usb, push_video, update_progress};
    struct cl_session *session = NULL;
    const char *result = "CarLife: Android accessory unavailable";
    uint64_t last_receive;
    int video_open = 0;
    (void)unused;

    b.phase_known = 0;
    if (aa_usb_open(quiet_log, usb_status, 20, &b.usb))
        goto done;
    session = cl_open(&io, 2);
    if (!session) {
        result = "CarLife: session allocation failed";
        goto done;
    }
    if (av_open(quiet_log, CL_WIDTH, CL_HEIGHT)) {
        result = "CarLife: CIC video ring unavailable";
        goto done;
    }
    video_open = 1;
    if (cl_start(session)) {
        result = cl_error(session);
        goto done;
    }
    last_receive = now_ms();

    for (;;) {
        int n, back, idr, reader_requests_idr;
        uint64_t now = now_ms();
        if (take_actions(&back, &idr)) {
            result = cl_pause(session) ? cl_error(session) : "CarLife disconnected";
            break;
        }
        if (b.phase != CL_STREAMING && now - b.phase_at > 15000) {
            result = status_text;
            break;
        }
        if (now - last_receive > 15000) {
            result = "CarLife: phone stopped sending";
            break;
        }
        reader_requests_idr = av_native_want_idr();
        if ((back && cl_back(session)) ||
            ((idr || reader_requests_idr) && cl_request_idr(session))) {
            result = cl_error(session);
            break;
        }
        n = b.usb.recv(b.usb.ctx, b.input, sizeof b.input, 200);
        if (n < 0) {
            result = "CarLife: USB closed";
            break;
        }
        if (n > 0) {
            last_receive = now_ms();
            if (cl_receive(session, b.input, (size_t)n)) {
                result = cl_error(session);
                break;
            }
        }
    }

done:
    if (video_open)
        av_close();
    aa_usb_close();
    cl_close(session);
    pthread_mutex_lock(&lock);
    result_text = status_text = result;
    busy = 0;
    pthread_mutex_unlock(&lock);
    return NULL;
}

int cicaa_start(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    int error;
    pthread_mutex_lock(&lock);
    if (busy) {
        pthread_mutex_unlock(&lock);
        return 1;
    }
    busy = 1;
    disconnect_requested = back_requested = idr_requested = 0;
    status_text = "CarLife: waiting for Android over USB";
    result_text = "";
    error = pthread_attr_init(&attr);
    if (!error) {
        error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (!error)
            error = pthread_attr_setstacksize(&attr, 128 * 1024);
        if (!error)
            error = pthread_create(&thread, &attr, worker, NULL);
        pthread_attr_destroy(&attr);
    }
    if (error) {
        busy = 0;
        result_text = status_text = "CarLife: could not start worker";
    }
    pthread_mutex_unlock(&lock);
    return error ? -1 : 0;
}

int cicaa_busy(void)
{
    int running;
    pthread_mutex_lock(&lock);
    running = busy;
    pthread_mutex_unlock(&lock);
    return running;
}

const char *cicaa_status(void)
{
    const char *text;
    pthread_mutex_lock(&lock);
    text = status_text;
    pthread_mutex_unlock(&lock);
    return text;
}

const char *cicaa_result(void)
{
    const char *text;
    pthread_mutex_lock(&lock);
    text = result_text;
    pthread_mutex_unlock(&lock);
    return text;
}

int cicaa_input(int kind, int code, int value)
{
    pthread_mutex_lock(&lock);
    if (busy && kind == CICAA_IN_BYEBYE)
        disconnect_requested = 1;
    if (busy && kind == CICAA_IN_KEY && code == 4 && value == 1)
        back_requested = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}

int cicaa_projection(void)
{
    return cicaa_busy();
}

void cicaa_project(void)
{
    pthread_mutex_lock(&lock);
    if (busy)
        idr_requested = 1;
    pthread_mutex_unlock(&lock);
}
