#!/bin/sh
# The desktop: ROX-Filer's pinboard (the backdrop and the icons left on
# it) and a panel along the bottom, as RISC OS's icon bar is, then JWM.
# Leaving JWM (Ctrl+Alt+Backspace, or its menu) ends the session.
# (rox sets itself going in the background for these.)
rox --pinboard=Default
rox --bottom=Default
# For ROX's own window manager instead, which iconifies windows onto the
# pinboard as RISC OS does: exec /usr/local/apps/OroboROX/AppRun
exec jwm
