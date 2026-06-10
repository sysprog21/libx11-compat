#include <X11/SM/SMlib.h>

SmcConn SmcOpenConnection(char *networkIdsList,
                          SmPointer context,
                          int xsmpMajorRev,
                          int xsmpMinorRev,
                          unsigned long mask,
                          SmcCallbacks *callbacks,
                          char *previousId,
                          char **clientIdRet,
                          int errorLength,
                          char *errorStringRet)
{
    (void) networkIdsList;
    (void) context;
    (void) xsmpMajorRev;
    (void) xsmpMinorRev;
    (void) mask;
    (void) callbacks;
    (void) previousId;
    (void) errorLength;
    if (clientIdRet)
        *clientIdRet = 0;
    if (errorStringRet && errorLength > 0)
        errorStringRet[0] = '\0';
    return 0;
}

SmcCloseStatus SmcCloseConnection(SmcConn smcConn,
                                  int count,
                                  char **reasonMsgs)
{
    (void) smcConn;
    (void) count;
    (void) reasonMsgs;
    return SmcClosedNow;
}

IceConn SmcGetIceConnection(SmcConn smcConn)
{
    (void) smcConn;
    return 0;
}

char *SmcClientID(SmcConn smcConn)
{
    (void) smcConn;
    return 0;
}

Status SmcRegisterClient(SmcConn smcConn,
                         SmPointer assignedID,
                         char **clientIdRet)
{
    (void) smcConn;
    (void) assignedID;
    if (clientIdRet)
        *clientIdRet = 0;
    return 0;
}

void SmcSetProperties(SmcConn smcConn,
                      int numProps,
                      SmProp **props)
{
    (void) smcConn;
    (void) numProps;
    (void) props;
}

void SmcDeleteProperties(SmcConn smcConn,
                         int numProps,
                         char **propNames)
{
    (void) smcConn;
    (void) numProps;
    (void) propNames;
}

void SmcSaveYourselfDone(SmcConn smcConn,
                         Bool success)
{
    (void) smcConn;
    (void) success;
}
