/* Stub X11/SM/SMlib.h for libx11-compat's libXt build
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

/* libXt's generated Shell.h unconditionally includes this header even
 * when session management is compiled out via XT_NO_SM. ShellP.h also
 * declares a SmcConn slot in the WidgetClass record so the struct layout
 * stays binary-compatible. libx11-compat ships no libSM and runs in a
 * single process with no XSMP peer, so the type lives only as an opaque
 * pointer here and the few constants that downstream code probes are
 * synthesized as enums.
 */
#ifndef _LIBX11_COMPAT_SMLIB_H_
#define _LIBX11_COMPAT_SMLIB_H_

#include <X11/ICE/ICElib.h>
#include <X11/Xfuncproto.h>
#include <X11/Xosdefs.h>

typedef struct _SmcConn *SmcConn;
typedef struct _SmsConn *SmsConn;
typedef IcePointer SmPointer;

typedef struct {
    int length;
    SmPointer value;
} SmPropValue;

typedef struct {
    char *name;
    char *type;
    int num_vals;
    SmPropValue *vals;
} SmProp;

typedef enum {
    SmcClosedNow = 0,
    SmcClosedASAP = 1,
    SmcConnectionInUse = 2
} SmcCloseStatus;

typedef void (*SmcSaveYourselfProc)(SmcConn smcConn,
                                    SmPointer clientData,
                                    int saveType,
                                    Bool shutdown,
                                    int interactStyle,
                                    Bool fast);
typedef void (*SmcDieProc)(SmcConn smcConn, SmPointer clientData);
typedef void (*SmcSaveCompleteProc)(SmcConn smcConn, SmPointer clientData);
typedef void (*SmcShutdownCancelledProc)(SmcConn smcConn, SmPointer clientData);

typedef struct {
    struct {
        SmcSaveYourselfProc callback;
        SmPointer client_data;
    } save_yourself;
    struct {
        SmcDieProc callback;
        SmPointer client_data;
    } die;
    struct {
        SmcSaveCompleteProc callback;
        SmPointer client_data;
    } save_complete;
    struct {
        SmcShutdownCancelledProc callback;
        SmPointer client_data;
    } shutdown_cancelled;
} SmcCallbacks;

/* Real libSM ships these as #define macros, so we mirror the same shape
 * with #ifndef guards. Using `enum { SmProtoMajor = 1, ... }` here would
 * conflict at preprocess time if any consumer also pulled in the system
 * <X11/SM/SMlib.h>, because its `#define SmProtoMajor 1` would expand
 * inside our enum declaration to `1 = 1`. */
#ifndef SmProtoMajor
#define SmProtoMajor 1
#endif
#ifndef SmProtoMinor
#define SmProtoMinor 0
#endif
#ifndef SmDialogError
#define SmDialogError 0
#endif
#ifndef SmDialogNormal
#define SmDialogNormal 1
#endif
#ifndef SmInteractStyleNone
#define SmInteractStyleNone 0
#endif
#ifndef SmInteractStyleErrors
#define SmInteractStyleErrors 1
#endif
#ifndef SmInteractStyleAny
#define SmInteractStyleAny 2
#endif
#ifndef SmSaveGlobal
#define SmSaveGlobal 0
#endif
#ifndef SmSaveLocal
#define SmSaveLocal 1
#endif
#ifndef SmSaveBoth
#define SmSaveBoth 2
#endif
#ifndef SmRestartIfRunning
#define SmRestartIfRunning 0
#endif
#ifndef SmRestartAnyway
#define SmRestartAnyway 1
#endif
#ifndef SmRestartImmediately
#define SmRestartImmediately 2
#endif
#ifndef SmRestartNever
#define SmRestartNever 3
#endif
#ifndef SmcSaveYourselfProcMask
#define SmcSaveYourselfProcMask 1
#endif
#ifndef SmcDieProcMask
#define SmcDieProcMask 2
#endif
#ifndef SmcSaveCompleteProcMask
#define SmcSaveCompleteProcMask 4
#endif
#ifndef SmcShutdownCancelledProcMask
#define SmcShutdownCancelledProcMask 8
#endif

#define SmCloneCommand "CloneCommand"
#define SmCurrentDirectory "CurrentDirectory"
#define SmDiscardCommand "DiscardCommand"
#define SmEnvironment "Environment"
#define SmProcessID "ProcessID"
#define SmProgram "Program"
#define SmResignCommand "ResignCommand"
#define SmRestartCommand "RestartCommand"
#define SmRestartStyleHint "RestartStyleHint"
#define SmShutdownCommand "ShutdownCommand"
#define SmUserID "UserID"

#define SmARRAY8 "ARRAY8"
#define SmCARD8 "CARD8"
#define SmLISTofARRAY8 "LISTofARRAY8"

_XFUNCPROTOBEGIN

SmcConn SmcOpenConnection(char *networkIdsList,
                          SmPointer context,
                          int xsmpMajorRev,
                          int xsmpMinorRev,
                          unsigned long mask,
                          SmcCallbacks *callbacks,
                          char *previousId,
                          char **clientIdRet,
                          int errorLength,
                          char *errorStringRet);
SmcCloseStatus SmcCloseConnection(SmcConn smcConn,
                                  int count,
                                  char **reasonMsgs);
IceConn SmcGetIceConnection(SmcConn smcConn);
char *SmcClientID(SmcConn smcConn);
Status SmcRegisterClient(SmcConn smcConn,
                         SmPointer assignedID,
                         char **clientIdRet);
void SmcSetProperties(SmcConn smcConn, int numProps, SmProp **props);
void SmcDeleteProperties(SmcConn smcConn, int numProps, char **propNames);
void SmcSaveYourselfDone(SmcConn smcConn, Bool success);

_XFUNCPROTOEND

#endif /* _LIBX11_COMPAT_SMLIB_H_ */
