#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#include <stdlib.h>

#include "util.h"

/* Safe no-op stubs for the X Synchronization extension. Clients that probe
 * for XSync (e.g. GTK, Qt) get a stable "not supported" answer rather than
 * crashing. */

Status XSyncQueryExtension(Display *dpy,
                           int *event_base_return,
                           int *error_base_return)
{
    (void) dpy;
    if (event_base_return)
        *event_base_return = 0;
    if (error_base_return)
        *error_base_return = 0;
    return False;
}

Status XSyncInitialize(Display *dpy,
                       int *major_version_return,
                       int *minor_version_return)
{
    (void) dpy;
    if (major_version_return)
        *major_version_return = 0;
    if (minor_version_return)
        *minor_version_return = 0;
    return False;
}

XSyncSystemCounter *XSyncListSystemCounters(Display *dpy,
                                            int *n_counters_return)
{
    (void) dpy;
    if (n_counters_return)
        *n_counters_return = 0;
    return NULL;
}

void XSyncFreeSystemCounterList(XSyncSystemCounter *list)
{
    free(list);
}

XSyncCounter XSyncCreateCounter(Display *dpy, XSyncValue initial_value)
{
    (void) dpy;
    (void) initial_value;
    return None;
}

Status XSyncSetCounter(Display *dpy, XSyncCounter counter, XSyncValue value)
{
    (void) dpy;
    (void) counter;
    (void) value;
    return False;
}

Status XSyncChangeCounter(Display *dpy, XSyncCounter counter, XSyncValue value)
{
    (void) dpy;
    (void) counter;
    (void) value;
    return False;
}

Status XSyncDestroyCounter(Display *dpy, XSyncCounter counter)
{
    (void) dpy;
    (void) counter;
    return False;
}

Status XSyncQueryCounter(Display *dpy,
                         XSyncCounter counter,
                         XSyncValue *value_return)
{
    (void) dpy;
    (void) counter;
    if (value_return) {
        value_return->hi = 0;
        value_return->lo = 0;
    }
    return False;
}

Status XSyncAwait(Display *dpy, XSyncWaitCondition *wait_list, int n_conditions)
{
    (void) dpy;
    (void) wait_list;
    (void) n_conditions;
    return False;
}

XSyncAlarm XSyncCreateAlarm(Display *dpy,
                            unsigned long values_mask,
                            XSyncAlarmAttributes *values)
{
    (void) dpy;
    (void) values_mask;
    (void) values;
    return None;
}

Status XSyncDestroyAlarm(Display *dpy, XSyncAlarm alarm)
{
    (void) dpy;
    (void) alarm;
    return False;
}

Status XSyncQueryAlarm(Display *dpy,
                       XSyncAlarm alarm,
                       XSyncAlarmAttributes *values_return)
{
    (void) dpy;
    (void) alarm;
    (void) values_return;
    return False;
}

Status XSyncChangeAlarm(Display *dpy,
                        XSyncAlarm alarm,
                        unsigned long values_mask,
                        XSyncAlarmAttributes *values)
{
    (void) dpy;
    (void) alarm;
    (void) values_mask;
    (void) values;
    return False;
}

Status XSyncSetPriority(Display *dpy, XID client_resource_id, int priority)
{
    (void) dpy;
    (void) client_resource_id;
    (void) priority;
    return False;
}

Status XSyncGetPriority(Display *dpy,
                        XID client_resource_id,
                        int *return_priority)
{
    (void) dpy;
    (void) client_resource_id;
    if (return_priority)
        *return_priority = 0;
    return False;
}

XSyncFence XSyncCreateFence(Display *dpy, Drawable d, Bool initially_triggered)
{
    (void) dpy;
    (void) d;
    (void) initially_triggered;
    return None;
}

Bool XSyncTriggerFence(Display *dpy, XSyncFence fence)
{
    (void) dpy;
    (void) fence;
    return False;
}

Bool XSyncResetFence(Display *dpy, XSyncFence fence)
{
    (void) dpy;
    (void) fence;
    return False;
}

Bool XSyncDestroyFence(Display *dpy, XSyncFence fence)
{
    (void) dpy;
    (void) fence;
    return False;
}

Bool XSyncQueryFence(Display *dpy, XSyncFence fence, Bool *triggered)
{
    (void) dpy;
    (void) fence;
    if (triggered)
        *triggered = False;
    return False;
}

Bool XSyncAwaitFence(Display *dpy, const XSyncFence *fence_list, int n_fences)
{
    (void) dpy;
    (void) fence_list;
    (void) n_fences;
    return False;
}
