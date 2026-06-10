#include <X11/ICE/ICElib.h>
#include <X11/SM/SMlib.h>

#include <stdio.h>

#define CHECK(cond, msg)        \
    do {                        \
        if (!(cond)) {          \
            puts("FAIL: " msg); \
            return 1;           \
        }                       \
    } while (0)

static void ice_io_error_handler(IceConn iceConn)
{
    (void) iceConn;
}

static void smc_save_yourself(SmcConn smcConn,
                              SmPointer clientData,
                              int saveType,
                              Bool shutdown,
                              int interactStyle,
                              Bool fast)
{
    (void) smcConn;
    (void) clientData;
    (void) saveType;
    (void) shutdown;
    (void) interactStyle;
    (void) fast;
}

int main(void)
{
    CHECK(IceInitThreads() == 0, "IceInitThreads should be a no-op failure");
    CHECK(IceConnectionNumber(0) == -1,
          "IceConnectionNumber should reject a null connection");
    int replyReady = 1;
    CHECK(IceProcessMessages(0, 0, &replyReady) ==
              IceProcessMessagesConnectionClosed,
          "IceProcessMessages should report a closed null connection");
    CHECK(replyReady == 0, "IceProcessMessages should clear reply-ready");
    IceIOErrorHandler oldHandler = IceSetIOErrorHandler(ice_io_error_handler);
    CHECK(oldHandler == 0,
          "IceSetIOErrorHandler should return previous handler");
    CHECK(IceSetIOErrorHandler(oldHandler) == ice_io_error_handler,
          "IceSetIOErrorHandler should retain previous handler");
    IceCloseStatus iceCloseStatus = IceCloseConnection(0);
    CHECK(iceCloseStatus == IceClosedNow,
          "IceCloseConnection should report no-op close");

    SmcCallbacks callbacks = {0};
    callbacks.save_yourself.callback = smc_save_yourself;
    callbacks.save_yourself.client_data = 0;
    char *clientId = (char *) 1;
    char error[8] = {'x'};
    CHECK(SmcOpenConnection(0, 0, SmProtoMajor, SmProtoMinor,
                            SmcSaveYourselfProcMask, &callbacks, 0, &clientId,
                            sizeof(error), error) == 0,
          "SmcOpenConnection should fail without a session manager");
    CHECK(clientId == 0, "SmcOpenConnection should clear client id");
    CHECK(error[0] == '\0', "SmcOpenConnection should clear error string");
    IceConn iceConn = SmcGetIceConnection(0);
    CHECK(iceConn == 0, "SmcGetIceConnection should reject a null connection");
    CHECK(SmcClientID(0) == 0, "SmcClientID should reject a null connection");
    char *registerClientId = (char *) 1;
    CHECK(SmcRegisterClient(0, 0, &registerClientId) == 0,
          "SmcRegisterClient should report no-op success");
    CHECK(!registerClientId,
          "SmcRegisterClient should clear the returned client id");
    SmcSetProperties(0, 0, 0);
    SmcDeleteProperties(0, 0, 0);
    SmcSaveYourselfDone(0, 0);
    SmcCloseStatus smcCloseStatus = SmcCloseConnection(0, 0, 0);
    CHECK(smcCloseStatus == SmcClosedNow,
          "SmcCloseConnection should report no-op close");

    puts("test_ice_sm_link: ok");
    return 0;
}
