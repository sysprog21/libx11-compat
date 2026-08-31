#ifndef LIBX11_COMPAT_XCB_PRIVATE_H
#define LIBX11_COMPAT_XCB_PRIVATE_H
#include <stdint.h>
#include <xcb/xcb.h>
typedef struct _XDisplay Display;
typedef union _XEvent XEvent;
Display *xcbCompatDisplay(xcb_connection_t *connection);
xcb_connection_t *xcbCompatConnectionForDisplay(Display *display);
void xcbCompatSetQueueOwner(xcb_connection_t *connection, int owner);
unsigned int xcbCompatEventWaiters(xcb_connection_t *connection);
void xcbCompatSetConnectionError(xcb_connection_t *connection, int error);
int xcbCompatRequestReady(xcb_connection_t *connection);
void xcbCompatReleaseRequestResources(xcb_connection_t *connection);
uint64_t xcbCompatNextSequence(xcb_connection_t *connection);
void xcbCompatSetNextSequence(xcb_connection_t *connection, uint64_t sequence);
void xcbCompatStorePending(xcb_connection_t *connection,
                           uint64_t sequence,
                           void *reply,
                           xcb_generic_error_t *error);
void xcbCompatStoreProtocolError(xcb_connection_t *connection,
                                 uint64_t sequence,
                                 uint8_t code,
                                 uint32_t resource,
                                 uint8_t opcode,
                                 int checked);
void *xcbCompatTakeReply(xcb_connection_t *connection,
                         uint64_t sequence,
                         xcb_generic_error_t **error);
xcb_void_cookie_t xcbCompatVoidCookie(xcb_connection_t *connection,
                                      uint8_t errorCode,
                                      uint32_t resource,
                                      uint8_t opcode,
                                      int checked);
#endif
