# Startup script sourced by magic::initialize for the layout replay smoke.
# Loads the local fixture and zooms it to fit so the layout window shows real
# multi-layer paint through libx11-compat. Does not quit: the replay engine
# needs the Tk/SDL event loop running to capture screenshots. A late safety
# exit keeps a hung run from waiting forever. It must clear the replay's own
# worst-case budget (delay + wait-window + two 20s wait-converge timeouts is
# ~57s before Tk-init and screenshot overhead), or a slow-but-healthy run
# trips the net mid-replay and reports a spurious "process exited" failure.
load minimal
view
after 180000 {exit 0}
