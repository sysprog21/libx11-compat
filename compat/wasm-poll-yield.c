/*
 * Browser poll/select yield bridge for unmodified toolkit event loops.
 *
 * libXt's _XtWaitForSomething blocks in poll(ConnectionNumber, ..., X_BLOCK)
 * (NextEvent.c, the USE_POLL path) before our XNextEvent -- and its idle-yield
 * hook -- is ever reached, so an unmodified XtAppMainLoop would freeze the
 * browser tab. An app links this object with -Wl,--wrap=poll,--wrap=select and
 * -sASYNCIFY. The wrapper turns a blocking or long wait into a probe with a
 * zero timeout: on "nothing ready" it pumps SDL (so browser input reaches our
 * event pipe, the fd libXt polls) then yields one browser turn and retries,
 * honoring a finite timeout via emscripten_get_now(). It works only because
 * ASYNCIFY instrumented the stack, which is why it lives in a separate object
 * the app links rather than in the core archive the non-ASYNCIFY examples use.
 *
 * pumpEventsSafe (src/events.c) drains the browser-delivered SDL events into
 * our queue and writes the wake byte to the event pipe; without driving it here
 * the pipe never becomes readable and poll would spin forever (the tab stays
 * responsive, but no events reach the app).
 *
 * An idle wait pumps and yields every emscripten_sleep(0) turn, which the
 * browser clamps to roughly 250 Hz. That is correct but not power-frugal for a
 * deployed app; if idle CPU ever matters, widen the sleep toward one animation
 * frame when the deadline (or the block-forever case) is far off.
 */
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <sys/select.h>

#include <emscripten.h>

extern void pumpEventsSafe(void);

int __real_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int __real_select(int nfds,
                  fd_set *readfds,
                  fd_set *writefds,
                  fd_set *exceptfds,
                  struct timeval *timeout);

int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    /* A non-blocking probe stays exactly that. */
    if (timeout == 0)
        return __real_poll(fds, nfds, 0);

    double deadline = timeout > 0 ? emscripten_get_now() + timeout : 0;
    for (;;) {
        pumpEventsSafe();
        int rc = __real_poll(fds, nfds, 0);
        if (rc != 0)
            return rc;
        if (timeout > 0 && emscripten_get_now() >= deadline)
            return 0;
        emscripten_sleep(0);
    }
}

int __wrap_select(int nfds,
                  fd_set *readfds,
                  fd_set *writefds,
                  fd_set *exceptfds,
                  struct timeval *timeout)
{
    /* select consumes the fd sets in place, so keep pristine copies to restore
     * before each zero-timeout probe.
     */
    fd_set r0, w0, e0;
    if (readfds)
        r0 = *readfds;
    if (writefds)
        w0 = *writefds;
    if (exceptfds)
        e0 = *exceptfds;

    /* A NULL timeout blocks until an fd is ready. A finite timeout is validated
     * and converted to milliseconds in double, so a sub-millisecond or
     * fractional wait is not truncated to a non-blocking probe and a large
     * tv_sec does not overflow a 32-bit long. An out-of-range timeval is
     * EINVAL, matching select. Only an exact {0,0} is the pure non-blocking
     * probe.
     */
    int block_forever = (timeout == NULL);
    double timeout_ms = 0;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
            timeout->tv_usec >= 1000000) {
            errno = EINVAL;
            return -1;
        }
        if (timeout->tv_sec == 0 && timeout->tv_usec == 0) {
            struct timeval zero = {0, 0};
            return __real_select(nfds, readfds, writefds, exceptfds, &zero);
        }
        timeout_ms = (double) timeout->tv_sec * 1000.0 +
                     (double) timeout->tv_usec / 1000.0;
    }

    double deadline = block_forever ? 0 : emscripten_get_now() + timeout_ms;
    for (;;) {
        pumpEventsSafe();
        fd_set r, w, e;
        if (readfds)
            r = r0;
        if (writefds)
            w = w0;
        if (exceptfds)
            e = e0;
        struct timeval zero = {0, 0};
        int rc = __real_select(nfds, readfds ? &r : NULL, writefds ? &w : NULL,
                               exceptfds ? &e : NULL, &zero);
        if (rc != 0) {
            if (readfds)
                *readfds = r;
            if (writefds)
                *writefds = w;
            if (exceptfds)
                *exceptfds = e;
            return rc;
        }
        if (!block_forever && emscripten_get_now() >= deadline) {
            if (readfds)
                FD_ZERO(readfds);
            if (writefds)
                FD_ZERO(writefds);
            if (exceptfds)
                FD_ZERO(exceptfds);
            return 0;
        }
        emscripten_sleep(0);
    }
}
