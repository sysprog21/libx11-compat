# Startup script sourced by magic::initialize for the layout replay smoke.
# Loads the local fixture and zooms it to fit so the layout window shows real
# multi-layer paint through libx11-compat. Does not quit: the replay engine
# needs the Tk/SDL event loop running to capture screenshots. A late safety
# exit keeps a hung run from waiting forever.
load minimal
view
after 60000 {exit 0}
