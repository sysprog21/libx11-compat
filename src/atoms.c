#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "atoms.h"
#include "atom-list.h"
#include "errors.h"
#include "display.h"

#define MAX_ATOM_VALUE ((Atom) UINT32_MAX)

AtomStruct *atomStorageStart = NULL;
static Atom lastUsedAtom = _NET_LAST_PREDEFINED;
static AtomStruct preDefAtomStructResult;

static Bool predefinedAtomMatchesName(const char *predefinedName,
                                      const char *name)
{
    if (strcmp(predefinedName, name) == 0)
        return True;
    /* The predefined list stores the C identifier (e.g. "XA_PRIMARY"); the X11
     * atom name strips the "XA_" prefix.
     */
    return strncmp(predefinedName, "XA_", 3) == 0 &&
           strcmp(&predefinedName[3], name) == 0;
}

AtomStruct *getAtomStruct(Atom atom)
{
    AtomStruct *atomStruct = atomStorageStart;
    while (atomStruct) {
        if (atomStruct->atom == atom)
            return atomStruct;
        atomStruct = atomStruct->next;
    }
    return NULL;
}

AtomStruct *getAtomStructByName(const char *name)
{
    size_t i;
    for (i = 0; i < PREDEFINED_ATOM_LIST_SIZE; i++) {
        if (predefinedAtomMatchesName(PredefinedAtomList[i].name, name)) {
            preDefAtomStructResult.atom = PredefinedAtomList[i].atom;
            preDefAtomStructResult.name = PredefinedAtomList[i].name;
            return &preDefAtomStructResult;
        }
    }
    AtomStruct *atomStruct = atomStorageStart;
    while (atomStruct) {
        if (strcmp(atomStruct->name, name) == 0)
            return atomStruct;
        atomStruct = atomStruct->next;
    }
    return NULL;
}

Bool isValidAtom(Atom atom)
{
    if (atom == None || atom > MAX_ATOM_VALUE)
        return False;
    if (atom <= _NET_LAST_PREDEFINED)
        return True;
    return getAtomStruct(atom) ? True : False;
}

const char *getAtomName(Display *display, Atom atom)
{
    (void) display;
    for (size_t i = 0; i < PREDEFINED_ATOM_LIST_SIZE; i++) {
        if (PredefinedAtomList[i].atom != atom)
            continue;
        const char *atomName = PredefinedAtomList[i].name;
        return strncmp(atomName, "XA_", 3) == 0 ? &atomName[3] : atomName;
    }
    AtomStruct *atomStruct = getAtomStruct(atom);
    return atomStruct ? atomStruct->name : NULL;
}

void freeAtomStorage()
{
    AtomStruct *atomStorage;
    while ((atomStorage = atomStorageStart)) {
        atomStorageStart = atomStorage->next;
        free((char *) atomStorage->name);
        free(atomStorage);
    }
}

char *XGetAtomName(Display *display, Atom atom)
{
    // https://tronche.com/gui/x/xlib/window-information/XGetAtomName.html
    SET_X_SERVER_REQUEST(display, X_GetAtomName);
    const char *atomName = getAtomName(display, atom);
    if (!atomName) {
        handleError(0, display, None, 0, BadAtom, 0);
        return NULL;
    }
    char *result = strdup(atomName);
    if (!result)
        handleOutOfMemory(0, display, 0, 0);
    return result;
}

/* Returns a nonzero status if names are returned for all of the atoms;
 * otherwise, it returns zero.
 */
Status XGetAtomNames(Display *dpy, Atom *atoms, int count, char **names_return)
{
    // https://tronche.com/gui/x/xlib/window-information/XGetAtomNames.html
    int returned_names = 0;
    for (int i = 0; i < count; ++i) {
        char *name = XGetAtomName(dpy, atoms[i]);
        if (name)
            returned_names++;
        names_return[i] = name;
    }
    return returned_names == count ? 1 : 0;
}

Atom _internAtom(const char *atomName, Bool only_if_exists, Bool *outOfMemory)
{
    if (outOfMemory)
        *outOfMemory = False;
    AtomStruct *existing = getAtomStructByName(atomName);
    if (existing)
        return existing->atom;
    if (only_if_exists)
        return None;

    if (lastUsedAtom >= MAX_ATOM_VALUE)
        goto oom;
    AtomStruct *atomStruct = malloc(sizeof(AtomStruct));
    if (!atomStruct)
        goto oom;
    atomStruct->name = strdup(atomName);
    if (!atomStruct->name) {
        free(atomStruct);
        goto oom;
    }
    atomStruct->atom = ++lastUsedAtom;
    atomStruct->next = atomStorageStart;
    atomStorageStart = atomStruct;
    return atomStruct->atom;

oom:
    if (outOfMemory)
        *outOfMemory = True;
    return None;
}

Atom internalInternAtom(const char *atomName)
{
    return _internAtom(atomName, False, NULL);
}

Atom XInternAtom(Display *display, _Xconst char *atom_name, Bool only_if_exists)
{
    // https://tronche.com/gui/x/xlib/window-information/XInternAtom.html
    SET_X_SERVER_REQUEST(display, X_InternAtom);
    if (!atom_name) {
        handleError(0, display, None, 0, BadName, 0);
        return None;
    }
    Bool outOfMemory;
    Atom result = _internAtom(atom_name, only_if_exists, &outOfMemory);
    if (outOfMemory)
        handleOutOfMemory(0, display, 0, 0);
    return result;
}

Status XInternAtoms(Display *dpy,
                    char **names,
                    int count,
                    Bool onlyIfExists,
                    Atom *atoms_return)
{
    // This function returns a nonzero status if atoms are returned for all of
    // the names; otherwise, it returns zero.
    int returned_atoms = 0;
    for (int i = 0; i < count; ++i) {
        Atom atom = XInternAtom(dpy, names[i], onlyIfExists);
        if (atom != None)
            returned_atoms++;
        atoms_return[i] = atom;
    }
    return returned_atoms == count ? 1 : 0;
}
