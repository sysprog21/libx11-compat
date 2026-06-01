#ifndef INPUT_H
#define INPUT_H

#include "X11/Xlib.h"
#include "window.h"

Window getKeyboardFocus();
void setKeyboardFocus(Window window);

#endif /* INPUT_H */
