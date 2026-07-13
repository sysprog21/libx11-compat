#include "mac-live-resize.h"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <stddef.h>
#include <string.h>

/* Minimal objc runtime decls (mirrors src/glx.c pinMetalLayerToPointSize). The
 * real <objc/runtime.h> pulls in <objc/objc.h>, whose bool/BOOL definitions
 * clash with headers elsewhere, so declare just the entry points used here and
 * cast objc_msgSend per call. This file deliberately includes neither SDL nor
 * X11 headers so its Cocoa/CoreGraphics interop stays isolated. */
typedef void *Class;
typedef void *SEL;
typedef signed char BOOL;
extern void *sel_registerName(const char *name);
extern void *objc_getClass(const char *name);
extern const char *object_getClassName(void *obj);
extern void objc_msgSend(void);
extern Class objc_allocateClassPair(Class superclass,
                                    const char *name,
                                    size_t extraBytes);
extern void objc_registerClassPair(Class cls);
extern BOOL class_addMethod(Class cls, SEL name, void *imp, const char *types);

/* One observer for the process, matching the single wake timer in events.c.
 * Registered from initEventPipe on the first display, removed from
 * closeEventPipe when the last display closes - both on the main thread, so no
 * additional synchronization is needed around this pointer.
 */
static CFRunLoopObserverRef liveResizeObserver = NULL;

/* Removing a CFRunLoopObserver from inside its own callback is unsafe (CF may
 * still dereference it after the callback returns). liveResizeObserverRunning
 * is set around the callback body; if a stop is requested while it is set (e.g.
 * a re-entrant XCloseDisplay teardown), liveResizeObserverPendingStop latches
 * the request and the real removal runs once the callback has returned. Both
 * are main-thread-only, matching the observer pointer itself. */
static int liveResizeObserverRunning = 0;
static int liveResizeObserverPendingStop = 0;

/* The NSWindow whose live resize armed the observer, captured from the
 * NSWindowWillStartLiveResize notification's object. Read each tick to query
 * -[NSWindow inLiveResize], AppKit's authoritative "the modal resize loop is
 * still running" flag: it stays YES for the whole drag - including a long pause
 * with the mouse button held - and flips to NO the instant the loop exits, even
 * if the NSWindowDidEndLiveResize notification is delayed (observed 30s+ late
 * in tracing). That makes it the correct disarm signal; a tick count or a
 * geometry-stable count cannot tell a paused-but-live drag from a finished one.
 * Main-thread-only, matching the observer pointer. */
static void *liveResizeWindow = NULL;

static void armLiveResizeObserver(void);
static void disarmLiveResizeObserver(void);

/* Query -[NSWindow inLiveResize] on the captured resize window. Returns 1 while
 * the modal resize loop is running, 0 when it has ended or when no window is
 * captured (so a missing/failed capture disarms rather than sticking armed).
 * BOOL is a signed char in the objc ABI; cast the IMP accordingly. */
static int liveResizeWindowInLiveResize(void)
{
    if (!liveResizeWindow)
        return 0;
    signed char inResize = ((signed char (*)(void *, void *)) objc_msgSend)(
        liveResizeWindow, sel_registerName("inLiveResize"));
    return inResize ? 1 : 0;
}

/* Fires on every iteration of the main run loop. During a normal frame the app
 * pumps SDL and repaints as usual, so the extra present here is a cheap no-op
 * (drawWindowDataToScreen returns immediately when nothing is dirty and the
 * window size is unchanged). During a live-resize drag the run loop is spinning
 * in the modal event-tracking mode with no SDL events flowing; this is the only
 * callback that still runs, so it is what keeps the window sharp.
 */
static void liveResizeObserverCallback(CFRunLoopObserverRef observer,
                                       CFRunLoopActivity activity,
                                       void *info)
{
    (void) observer;
    (void) activity;
    (void) info;
    liveResizeObserverRunning = 1;
    libx11CompatLiveResizeTrace("tick-in");
    libx11CompatPresentDuringLiveResize();
    libx11CompatLiveResizeTrace("tick-out");
    /* A re-entrant XCloseDisplay teardown inside the present above can latch a
     * pending stop and destroy the captured NSWindow. Honour the latch first,
     * before touching liveResizeWindow, so inLiveResize is never messaged on a
     * freed handle. Keep liveResizeObserverRunning set until after every disarm
     * decision below so any disarmLiveResizeObserver() reached from inside this
     * callback still routes through the pending-stop latch rather than removing
     * the observer synchronously while CF may still dereference it. */
    if (liveResizeObserverPendingStop) {
        liveResizeObserverPendingStop = 0;
        liveResizeObserverRunning = 0;
        disarmLiveResizeObserver(); /* now safe: outside body */
        return;
    }
    /* Ask AppKit whether the modal resize loop is still running. This is the
     * authoritative end-of-drag signal: it survives a long mid-drag pause
     * (stays YES) yet flips to NO the moment the loop exits, so we disarm
     * exactly then instead of waiting for a possibly-delayed DidEnd
     * notification. */
    int stillResizing = liveResizeWindowInLiveResize();
    if (!stillResizing) {
        /* Modal loop has ended (DidEnd may not have posted yet). Disarm and
         * present one final, fully-reflowed frame so the window settles on
         * correct content and the observer stops ticking. The disarm defers to
         * the pending-stop latch because liveResizeObserverRunning is still
         * set; clear it and honour the latch after the final present so the
         * removal lands once this callback has fully returned. */
        libx11CompatLiveResizeTrace("observer\tinactive-disarm");
        disarmLiveResizeObserver();
        libx11CompatPresentDuringLiveResizeEx(1);
        liveResizeObserverRunning = 0;
        if (liveResizeObserverPendingStop) {
            liveResizeObserverPendingStop = 0;
            disarmLiveResizeObserver();
        }
        return;
    }
    liveResizeObserverRunning = 0;
}

/* Add the CFRunLoopObserver to the main run loop. Called from the
 * NSWindowWillStartLiveResize handler so the per-tick present only fires while
 * a drag is actually in progress; idempotent (a no-op if already armed). */
static void armLiveResizeObserver(void)
{
    /* Fresh drag: clear last-seen size + settle counter so the first ticks of
     * this drag don't inherit a stale "settled" state from the previous one. */
    libx11CompatResetLiveResizeCoalesce();
    if (liveResizeObserver)
        return;
    CFRunLoopRef runLoop = CFRunLoopGetMain();
    if (!runLoop)
        runLoop = CFRunLoopGetCurrent();
    if (!runLoop)
        return;
    /* kCFRunLoopBeforeWaiting fires after each pass through the sources, right
     * before the loop would block. In the modal resize loop the tracking timer
     * keeps waking it, so this fires on every drag tick. Observing the common
     * modes attaches to whichever mode the loop currently runs, including the
     * event-tracking mode that the resize drag pushes.
     */
    liveResizeObserver = CFRunLoopObserverCreate(
        kCFAllocatorDefault, kCFRunLoopBeforeWaiting, true /* repeats */,
        0 /* order */, liveResizeObserverCallback, NULL);
    if (!liveResizeObserver)
        return;
    CFRunLoopAddObserver(runLoop, liveResizeObserver, kCFRunLoopCommonModes);
    libx11CompatLiveResizeTrace("observer\tarm");
}

/* Remove the CFRunLoopObserver from the main run loop. Called from the
 * NSWindowDidEndLiveResize handler and from teardown. Removing the observer
 * from inside its own callback is unsafe (CF may still dereference it after the
 * callback returns), so when that happens the pending-stop latch defers the
 * real removal to the end of the callback. Idempotent. */
static void disarmLiveResizeObserver(void)
{
    if (!liveResizeObserver)
        return;
    if (liveResizeObserverRunning) {
        /* Called from within the observer callback (via a re-entrant
         * XCloseDisplay teardown). Defer the removal to the end of the callback
         * so CF does not dereference a freed observer after we return. */
        liveResizeObserverPendingStop = 1;
        return;
    }
    CFRunLoopRef runLoop = CFRunLoopGetMain();
    if (!runLoop)
        runLoop = CFRunLoopGetCurrent();
    if (runLoop)
        CFRunLoopRemoveObserver(runLoop, liveResizeObserver,
                                kCFRunLoopCommonModes);
    CFRelease(liveResizeObserver);
    liveResizeObserver = NULL;
    /* Drag is over: drop the window reference so inLiveResize is never queried
     * on a stale handle before the next will-start recaptures it. */
    liveResizeWindow = NULL;
    libx11CompatLiveResizeTrace("observer\tdisarm");
}

/* NSNotification handlers. NSWindow posts NSWindowWillStartLiveResize when a
 * border-drag begins and NSWindowDidEndLiveResize when the mouse is released -
 * the canonical AppKit signals for the modal resize loop. We arm the run-loop
 * observer only for that interval, so it never ticks (and never keeps the
 * resize cursor / "in-resize" state alive) after the drag ends. The end handler
 * also fires one final present so the window lands on a correct 1:1 frame.
 *
 * The methods take (self, _cmd, notification) - the standard objc IMP shape for
 * a single-argument selector. self/_cmd are unused; will-start reads the
 * notification's object (the resizing NSWindow) so the observer can poll its
 * inLiveResize flag. This app has one top-level. */
static void liveResizeWillStart(void *self, void *_cmd, void *notification)
{
    (void) self;
    (void) _cmd;
    /* -[NSNotification object] is the NSWindow that began the live resize. Keep
     * it so each observer tick can ask AppKit whether the drag is still live.
     */
    liveResizeWindow = notification
                           ? ((void *(*) (void *, void *) ) objc_msgSend)(
                                 notification, sel_registerName("object"))
                           : NULL;
    libx11CompatLiveResizeTrace("notify\twill-start");
    armLiveResizeObserver();
}

static void liveResizeDidEnd(void *self, void *_cmd, void *notification)
{
    (void) self;
    (void) _cmd;
    (void) notification;
    libx11CompatLiveResizeTrace("notify\tdid-end");
    /* A DidEnd can arrive late - AppKit has been seen posting it 30s+ after the
     * modal loop exits, by which point the observer callback has already
     * disarmed off -[NSWindow inLiveResize] and a new drag may have re-armed
     * and captured a new window. If the captured window is still in a live
     * resize, this DidEnd belongs to a finished earlier drag; disarming now
     * would kill the active drag's per-tick reflow/presents, so ignore it.
     * Likewise, if the observer is already gone the earlier drag was fully
     * finalized and there is nothing to do. */
    if (liveResizeWindowInLiveResize())
        return;
    if (!liveResizeObserver)
        return;
    disarmLiveResizeObserver();
    /* One last present outside the modal loop so the final geometry lands on
     * fully reflowed content 1:1 (the observer is now removed, so nothing else
     * will repaint this frame). forceReflow bypasses the settle wait so the
     * final size always gets a real reflow even if the drag ended mid-move. */
    libx11CompatPresentDuringLiveResizeEx(1);
}

/* Lazily create a tiny NSObject subclass with the two notification-handler
 * methods and return a registered instance. Runtime-only (no @interface), so
 * this stays inside the existing objc_msgSend interop with no framework link.
 * Returns NULL if the runtime calls fail. Cached for the process. */
static void *liveResizeNotificationTarget(void)
{
    static void *instance = NULL;
    if (instance)
        return instance;
    Class superclass = objc_getClass("NSObject");
    if (!superclass)
        return NULL;
    Class cls =
        objc_allocateClassPair(superclass, "LibX11CompatLiveResizeTarget", 0);
    if (!cls) {
        /* Already registered from a prior init/teardown cycle: reuse it. */
        cls = objc_getClass("LibX11CompatLiveResizeTarget");
        if (!cls)
            return NULL;
    } else {
        /* "v@:@" = void return, self (id), _cmd (SEL), one object argument. */
        class_addMethod(cls, sel_registerName("onWillStartLiveResize:"),
                        (void *) liveResizeWillStart, "v@:@");
        class_addMethod(cls, sel_registerName("onDidEndLiveResize:"),
                        (void *) liveResizeDidEnd, "v@:@");
        objc_registerClassPair(cls);
    }
    void *obj = ((void *(*) (void *, void *) ) objc_msgSend)(
        cls, sel_registerName("alloc"));
    if (!obj)
        return NULL;
    instance = ((void *(*) (void *, void *) ) objc_msgSend)(
        obj, sel_registerName("init"));
    return instance;
}

/* Register the process's notification target with the default notification
 * center for the two live-resize notifications. Passing a nil object means "any
 * window", which is what we want (one process-wide observer). Idempotent via
 * the cached target + a registration latch. */
static int liveResizeNotificationsRegistered = 0;

static void registerLiveResizeNotifications(void)
{
    if (liveResizeNotificationsRegistered)
        return;
    void *target = liveResizeNotificationTarget();
    if (!target)
        return;
    void *ncCls = objc_getClass("NSNotificationCenter");
    if (!ncCls)
        return;
    void *center = ((void *(*) (void *, void *) ) objc_msgSend)(
        ncCls, sel_registerName("defaultCenter"));
    if (!center)
        return;
    void *willName = ((void *(*) (void *, void *, const char *) ) objc_msgSend)(
        objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"),
        "NSWindowWillStartLiveResizeNotification");
    void *endName = ((void *(*) (void *, void *, const char *) ) objc_msgSend)(
        objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"),
        "NSWindowDidEndLiveResizeNotification");
    if (!willName || !endName)
        return;
    SEL add = sel_registerName("addObserver:selector:name:object:");
    ((void (*)(void *, void *, void *, void *, void *, void *)) objc_msgSend)(
        center, add, target, sel_registerName("onWillStartLiveResize:"),
        willName, NULL);
    ((void (*)(void *, void *, void *, void *, void *, void *)) objc_msgSend)(
        center, add, target, sel_registerName("onDidEndLiveResize:"), endName,
        NULL);
    liveResizeNotificationsRegistered = 1;
    libx11CompatLiveResizeTrace("notify\tregistered");
}

static void unregisterLiveResizeNotifications(void)
{
    if (!liveResizeNotificationsRegistered)
        return;
    void *target = liveResizeNotificationTarget();
    void *ncCls = objc_getClass("NSNotificationCenter");
    if (target && ncCls) {
        void *center = ((void *(*) (void *, void *) ) objc_msgSend)(
            ncCls, sel_registerName("defaultCenter"));
        if (center)
            ((void (*)(void *, void *, void *)) objc_msgSend)(
                center, sel_registerName("removeObserver:"), target);
    }
    liveResizeNotificationsRegistered = 0;
}

void libx11CompatStartLiveResizeObserver(void)
{
    /* Do NOT arm the run-loop observer here. Register for the AppKit
     * live-resize notifications instead; the observer is armed only for the
     * duration of an actual drag (WillStart..DidEnd), so it never ticks in
     * steady state. */
    registerLiveResizeNotifications();
}

void libx11CompatStopLiveResizeObserver(void)
{
    /* Teardown / re-entrant close: stop listening and make sure the run-loop
     * observer is removed even if a drag was somehow still in progress. */
    unregisterLiveResizeNotifications();
    disarmLiveResizeObserver();
}

/* Glitch-free live-resize configuration (Phase 2B).
 *
 * During the modal live-resize loop macOS delivers no SDL events, so nothing
 * repaints; the compositor lags the app by one or more frames and, by default
 * (contentsGravity = kCAGravityResize), stretches the last drawable to fill the
 * growing window bounds - visible glyph distortion until the next real frame.
 *
 * The fix is NOT to upload pixels to the layer (assigning a CGImage to a
 * CAMetalLayer fights its internal CAImageQueue: red tint, broken modal-loop
 * exit, stuck resize cursor). It is two one-time configuration calls on SDL's
 * own CAMetalLayer, leaving contents untouched so SDL keeps presenting real
 * frames via nextDrawable:
 *
 *   contentsGravity = kCAGravityTopLeft  - pins the stale drawable top-left
 *       instead of scaling it, so the un-reflowed grown edge is clipped rather
 *       than stretched (no blur).
 *   contentsScale   = backingScaleFactor - maps drawable pixels 1:1 to screen
 *       pixels on Retina, without which topLeft gravity mispositions content.
 *
 * Battle-tested pattern (Tristan Hume 2019; the metal-live-resize crate), which
 * also notes presentsWithTransaction + waitUntilScheduled breaks frame delivery
 * whereas gravity + scale alone suffices and does not block the event loop.
 * Because we never replace the layer's contents, there is no drag-end cleanup
 * and no black window; the topLeft gravity is harmless in steady state (the
 * drawable always matches bounds outside a resize). Idempotent. */

/* Resolve SDL's own CAMetalLayer for the window.
 *
 * On this stack (sdl2-compat over SDL3) the renderer's Metal output lives in a
 * CAMetalLayer that is a sublayer of the content view's root
 * NSViewBackingLayer:
 *
 *   NSViewBackingLayer            <- [[nswindow contentView] layer]
 *     [0] CAMetalLayer            <- SDL's render target (returned)
 *
 * Walks the root's sublayers and returns the first whose class is CAMetalLayer;
 * falls back to the root layer if none is found (safe for non-Metal backends).
 * Returns NULL if the handle is not a Cocoa window or has no layer yet. */
static void *windowMetalLayer(void *nsWindow)
{
    if (!nsWindow)
        return NULL;
    void *contentView = ((void *(*) (void *, void *) ) objc_msgSend)(
        nsWindow, sel_registerName("contentView"));
    if (!contentView)
        return NULL;
    void *root = ((void *(*) (void *, void *) ) objc_msgSend)(
        contentView, sel_registerName("layer"));
    if (!root)
        return NULL;
    void *sublayers = ((void *(*) (void *, void *) ) objc_msgSend)(
        root, sel_registerName("sublayers"));
    if (sublayers) {
        long count = ((long (*)(void *, void *)) objc_msgSend)(
            sublayers, sel_registerName("count"));
        for (long i = 0; i < count; i++) {
            void *layer = ((void *(*) (void *, void *, long) ) objc_msgSend)(
                sublayers, sel_registerName("objectAtIndex:"), i);
            const char *cls = layer ? object_getClassName(layer) : NULL;
            if (cls && strcmp(cls, "CAMetalLayer") == 0)
                return layer;
        }
    }
    return root;
}

/* Wrap layer mutations in a CATransaction with implicit animations disabled, so
 * each live-resize frame swaps instantly (no cross-fade) and does not queue an
 * animation on the modal run loop. */
static void beginInstantTransaction(void)
{
    void *cls = objc_getClass("CATransaction");
    if (!cls)
        return;
    ((void (*)(void *, void *)) objc_msgSend)(cls, sel_registerName("begin"));
    ((void (*)(void *, void *, int)) objc_msgSend)(
        cls, sel_registerName("setDisableActions:"), 1);
}

static void commitInstantTransaction(void)
{
    void *cls = objc_getClass("CATransaction");
    if (!cls)
        return;
    ((void (*)(void *, void *)) objc_msgSend)(cls, sel_registerName("commit"));
}

/* Build an NSString for the topLeft gravity constant from its literal value.
 * kCAGravityTopLeft is documented to equal @"topLeft"; constructing it via the
 * objc runtime avoids resolving the QuartzCore symbol / linking the framework.
 * Cached after first use. Under manual retain/release (no ARC) the cache must
 * own a +1 reference: +stringWithUTF8String: returns an autoreleased instance
 * that the surrounding autorelease pool drains every run-loop tick, which would
 * leave the static dangling. +alloc/-initWithUTF8String: returns a retained
 * (+1) instance that is never autoreleased, so the process-lifetime cache stays
 * valid; the object is intentionally never released. */
static void *topLeftGravityString(void)
{
    static void *cached = NULL;
    if (cached)
        return cached;
    void *cls = objc_getClass("NSString");
    if (!cls)
        return NULL;
    void *allocated = ((void *(*) (void *, void *) ) objc_msgSend)(
        cls, sel_registerName("alloc"));
    if (!allocated)
        return NULL;
    cached = ((void *(*) (void *, void *, const char *) ) objc_msgSend)(
        allocated, sel_registerName("initWithUTF8String:"), "topLeft");
    return cached;
}

void libx11CompatConfigureLiveResizeLayer(void *nsWindow)
{
    void *layer = windowMetalLayer(nsWindow);
    if (!layer) {
        libx11CompatLiveResizeTrace("configure\tno-layer");
        return;
    }
    /* backingScaleFactor is the window's points->pixels ratio (2.0 on Retina).
     * The CAMetalLayer's drawable is sized in physical pixels, so contentsScale
     * must match for topLeft gravity to position content correctly; without it
     * the pinned drawable lands at the wrong spot on Retina. Returns a CGFloat
     * (double on 64-bit) by value. */
    double scale = ((double (*)(void *, void *)) objc_msgSend)(
        nsWindow, sel_registerName("backingScaleFactor"));
    if (scale <= 0.0)
        scale = 1.0;
    /* Trace once per process (not per frame) so the resolved scale is visible
     * for the black-edge/misposition diagnosis without spamming every tick. */
    static int tracedScale = 0;
    if (!tracedScale) {
        tracedScale = 1;
        libx11CompatLiveResizeTrace(scale >= 1.5 ? "configure\tscale~2"
                                                 : "configure\tscale~1");
    }
    void *gravity = topLeftGravityString();
    /* One-time, idempotent. Wrap in an instant transaction so the change never
     * animates; contents is left untouched so SDL keeps presenting real frames
     * via nextDrawable and there is no drag-end cleanup / black window. */
    beginInstantTransaction();
    if (gravity)
        ((void (*)(void *, void *, void *)) objc_msgSend)(
            layer, sel_registerName("setContentsGravity:"), gravity);
    ((void (*)(void *, void *, double)) objc_msgSend)(
        layer, sel_registerName("setContentsScale:"), scale);
    commitInstantTransaction();
}

#else /* !__APPLE__ */

void libx11CompatStartLiveResizeObserver(void) {}
void libx11CompatStopLiveResizeObserver(void) {}
void libx11CompatConfigureLiveResizeLayer(void *nsWindow)
{
    (void) nsWindow;
}

#endif /* __APPLE__ */
