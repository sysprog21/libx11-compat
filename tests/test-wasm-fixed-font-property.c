#include <stdio.h>

extern int x11compat_wasm_fixed_font_property_selftest(void);

int main(void)
{
    int failed = x11compat_wasm_fixed_font_property_selftest();
    if (failed)
        fprintf(stderr, "fixed bitmap XA_FONT property self-test failed\n");
    return failed;
}
