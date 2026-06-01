#ifndef _SYNC_H_
#define _SYNC_H_

#include <X11/Xlib.h>

#define SYNC_MAJOR_VERSION 3
#define SYNC_MINOR_VERSION 1

typedef XID XSyncCounter;
typedef XID XSyncAlarm;
typedef XID XSyncFence;

typedef struct _XSyncValue {
    int hi;
    unsigned int lo;
} XSyncValue;

typedef enum { XSyncAbsolute, XSyncRelative } XSyncValueType;

typedef enum {
    XSyncPositiveTransition,
    XSyncNegativeTransition,
    XSyncPositiveComparison,
    XSyncNegativeComparison
} XSyncTestType;

typedef enum {
    XSyncAlarmActive,
    XSyncAlarmInactive,
    XSyncAlarmDestroyed
} XSyncAlarmState;

typedef struct {
    XSyncCounter counter;
    XSyncValueType value_type;
    XSyncValue wait_value;
    XSyncTestType test_type;
} XSyncTrigger;

typedef struct {
    XSyncTrigger trigger;
    XSyncValue event_threshold;
} XSyncWaitCondition;

typedef struct {
    XSyncTrigger trigger;
    XSyncValue delta;
    Bool events;
    XSyncAlarmState state;
} XSyncAlarmAttributes;

typedef struct {
    XSyncCounter counter;
    char *name;
    XSyncValue resolution;
} XSyncSystemCounter;

extern Status XSyncQueryExtension(Display *dpy,
                                  int *event_base_return,
                                  int *error_base_return);
extern Status XSyncInitialize(Display *dpy,
                              int *major_version_return,
                              int *minor_version_return);
extern XSyncSystemCounter *XSyncListSystemCounters(Display *dpy,
                                                   int *n_counters_return);
extern void XSyncFreeSystemCounterList(XSyncSystemCounter *list);
extern XSyncCounter XSyncCreateCounter(Display *dpy, XSyncValue initial_value);
extern Status XSyncSetCounter(Display *dpy,
                              XSyncCounter counter,
                              XSyncValue value);
extern Status XSyncChangeCounter(Display *dpy,
                                 XSyncCounter counter,
                                 XSyncValue value);
extern Status XSyncDestroyCounter(Display *dpy, XSyncCounter counter);
extern Status XSyncQueryCounter(Display *dpy,
                                XSyncCounter counter,
                                XSyncValue *value_return);
extern Status XSyncAwait(Display *dpy,
                         XSyncWaitCondition *wait_list,
                         int n_conditions);
extern XSyncAlarm XSyncCreateAlarm(Display *dpy,
                                   unsigned long values_mask,
                                   XSyncAlarmAttributes *values);
extern Status XSyncDestroyAlarm(Display *dpy, XSyncAlarm alarm);
extern Status XSyncQueryAlarm(Display *dpy,
                              XSyncAlarm alarm,
                              XSyncAlarmAttributes *values_return);
extern Status XSyncChangeAlarm(Display *dpy,
                               XSyncAlarm alarm,
                               unsigned long values_mask,
                               XSyncAlarmAttributes *values);
extern Status XSyncSetPriority(Display *dpy,
                               XID client_resource_id,
                               int priority);
extern Status XSyncGetPriority(Display *dpy,
                               XID client_resource_id,
                               int *return_priority);
extern XSyncFence XSyncCreateFence(Display *dpy,
                                   Drawable d,
                                   Bool initially_triggered);
extern Bool XSyncTriggerFence(Display *dpy, XSyncFence fence);
extern Bool XSyncResetFence(Display *dpy, XSyncFence fence);
extern Bool XSyncDestroyFence(Display *dpy, XSyncFence fence);
extern Bool XSyncQueryFence(Display *dpy, XSyncFence fence, Bool *triggered);
extern Bool XSyncAwaitFence(Display *dpy,
                            const XSyncFence *fence_list,
                            int n_fences);

#endif /* _SYNC_H_ */
