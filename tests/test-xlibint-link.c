/* Downstream link check for private ABI declared by installed Xlibint.h. */
#include <X11/Xlibint.h>
#include <stdio.h>

int main(void)
{
    char hostname[256];
    int (*default_error)(Display *, XErrorEvent *) = _XDefaultError;

    if (!default_error) {
        fprintf(stderr, "_XDefaultError did not link\n");
        return 1;
    }
    if (_XGetHostname(hostname, (int) sizeof(hostname)) < 0) {
        fprintf(stderr, "_XGetHostname failed\n");
        return 1;
    }

    printf("test_xlibint_link: ok\n");
    return 0;
}
