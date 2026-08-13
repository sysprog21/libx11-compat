#ifndef _NET_ATOMS_H_
#define _NET_ATOMS_H_
// XA_LAST_PREDEFINED is 68

#define _NET_WM_NAME ((Atom) 69)
#define _NET_WM_VISIBLE_NAME ((Atom) 70)
#define _NET_WM_ICON_NAME ((Atom) 71)
#define _NET_WM_VISIBLE_ICON_NAME ((Atom) 72)
#define _NET_WM_DESKTOP ((Atom) 73)
#define _NET_WM_WINDOW_TYPE ((Atom) 74)
#define _NET_WM_WINDOW_TYPE_DESKTOP ((Atom) 75)
#define _NET_WM_WINDOW_TYPE_DOCK ((Atom) 76)
#define _NET_WM_WINDOW_TYPE_TOOLBAR ((Atom) 77)
#define _NET_WM_WINDOW_TYPE_MENU ((Atom) 78)
#define _NET_WM_WINDOW_TYPE_UTILITY ((Atom) 79)
#define _NET_WM_WINDOW_TYPE_SPLASH ((Atom) 80)
#define _NET_WM_WINDOW_TYPE_DIALOG ((Atom) 81)
#define _NET_WM_WINDOW_TYPE_DROPDOWN_MENU ((Atom) 82)
#define _NET_WM_WINDOW_TYPE_POPUP_MENU ((Atom) 83)
#define _NET_WM_WINDOW_TYPE_TOOLTIP ((Atom) 84)
#define _NET_WM_WINDOW_TYPE_NOTIFICATION ((Atom) 85)
#define _NET_WM_WINDOW_TYPE_COMBO ((Atom) 86)
#define _NET_WM_WINDOW_TYPE_DND ((Atom) 87)
#define _NET_WM_WINDOW_TYPE_NORMAL ((Atom) 88)
#define _NET_WM_STATE ((Atom) 89)
#define _NET_WM_STATE_MODAL ((Atom) 90)
#define _NET_WM_STATE_STICKY ((Atom) 91)
#define _NET_WM_STATE_MAXIMIZED_VERT ((Atom) 92)
#define _NET_WM_STATE_MAXIMIZED_HORZ ((Atom) 93)
#define _NET_WM_STATE_SHADED ((Atom) 94)
#define _NET_WM_STATE_SKIP_TASKBAR ((Atom) 95)
#define _NET_WM_STATE_SKIP_PAGER ((Atom) 96)
#define _NET_WM_STATE_HIDDEN ((Atom) 97)
#define _NET_WM_STATE_FULLSCREEN ((Atom) 98)
#define _NET_WM_STATE_ABOVE ((Atom) 99)
#define _NET_WM_STATE_BELOW ((Atom) 100)
#define _NET_WM_STATE_DEMANDS_ATTENTION ((Atom) 101)
#define _NET_WM_STATE_REMOVE ((Atom) 102)
#define _NET_WM_STATE_ADD ((Atom) 103)
#define _NET_WM_STATE_TOGGLE ((Atom) 104)
#define _NET_WM_ALLOWED_ACTIONS ((Atom) 105)
#define _NET_WM_ACTION_MOVE ((Atom) 106)
#define _NET_WM_ACTION_RESIZE ((Atom) 107)
#define _NET_WM_ACTION_MINIMIZE ((Atom) 108)
#define _NET_WM_ACTION_SHADE ((Atom) 109)
#define _NET_WM_ACTION_STICK ((Atom) 110)
#define _NET_WM_ACTION_MAXIMIZE_HORZ ((Atom) 111)
#define _NET_WM_ACTION_MAXIMIZE_VERT ((Atom) 112)
#define _NET_WM_ACTION_FULLSCREEN ((Atom) 113)
#define _NET_WM_ACTION_CHANGE_DESKTOP ((Atom) 114)
#define _NET_WM_ACTION_CLOSE ((Atom) 115)
#define _NET_WM_ACTION_ABOVE ((Atom) 116)
#define _NET_WM_ACTION_BELOW ((Atom) 117)
#define _NET_WM_STRUT ((Atom) 118)
#define _NET_WM_STRUT_PARTIAL ((Atom) 119)
#define _NET_WM_ICON_GEOMETRY ((Atom) 120)
#define _NET_WM_ICON ((Atom) 121)
#define _NET_WM_PID ((Atom) 122)
#define _NET_WM_HANDLED_ICONS ((Atom) 123)
#define _NET_WM_USER_TIME ((Atom) 124)
#define _NET_WM_USER_TIME_WINDOW ((Atom) 125)
#define _NET_FRAME_EXTENTS ((Atom) 126)
#define _MOTIF_WM_HINTS ((Atom) 127)
#define _XSETTINGS_SETTINGS_ATOM ((Atom) 128)

/* ICCCM WM_PROTOCOLS / WM_DELETE_WINDOW are not in the core Xatom.h predefined
 * set, but the in-process WM interns them at display init and the state
 * snapshot tracks WM_PROTOCOLS by compile-time constant. Pinning fixed ids here
 * guarantees the value a client stores equals the value the snapshot looks up,
 * instead of a dynamically assigned id that would drift.
 */
#define WM_PROTOCOLS ((Atom) 129)
#define WM_DELETE_WINDOW ((Atom) 130)

/* UTF8_STRING and COMPOUND_TEXT are text-property encodings every modern
 * toolkit writes, and the WM_NAME / _NET_WM_NAME title path compares against
 * both on every write. Interned dynamically they were the worst case in
 * XInternAtom: absent from this table, so each lookup scanned all of it and
 * then walked the dynamic list to its end (measured around 2us per call, and
 * growing with the client's atom count). Pinning ids turns those comparisons
 * into integer tests and removes the reason the call sites had to re-resolve
 * per write to avoid dangling across freeAtomStorage.
 */
#define UTF8_STRING ((Atom) 131)
#define COMPOUND_TEXT ((Atom) 132)

#define _NET_LAST_PREDEFINED ((Atom) 132)

#endif /* _NET_ATOMS_H_ */
