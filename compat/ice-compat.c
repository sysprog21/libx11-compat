#include <X11/ICE/ICElib.h>

int IceInitThreads(void)
{
    return 0;
}

int IceConnectionNumber(IceConn iceConn)
{
    (void) iceConn;
    return -1;
}

IceProcessMessagesStatus IceProcessMessages(IceConn iceConn,
                                            IceReplyWaitInfo *replyWait,
                                            Bool *replyReadyRet)
{
    (void) iceConn;
    (void) replyWait;
    if (replyReadyRet)
        *replyReadyRet = 0;
    return IceProcessMessagesConnectionClosed;
}

IceIOErrorHandler IceSetIOErrorHandler(IceIOErrorHandler handler)
{
    static IceIOErrorHandler currentHandler;
    IceIOErrorHandler previousHandler = currentHandler;
    currentHandler = handler;
    return previousHandler;
}

IceCloseStatus IceCloseConnection(IceConn iceConn)
{
    (void) iceConn;
    return IceClosedNow;
}
