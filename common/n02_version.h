#pragma once

// Preprocessor stringification helpers.
#define N02_STRINGIFY(x) #x
#define N02_TOSTRING(x) N02_STRINGIFY(x)

// Single source of truth for the build version, shown across every dialog
// that needs it (About box, connection/lobby window titles, P2P/Kaillera
// handshake "Using version: ..." chat messages, etc).
//
// Set from CI via GIT_REVISION (see .github/workflows/build.yml), which
// comes from `git describe --tags --always` - release tags follow the
// "v.TIERES.X.Y" pattern (e.g. v.TIERES.0.11), so a CI build's version
// string reads e.g. "v.TIERES.0.11" or "v.TIERES.0.11-3-gabcdef" for commits
// past the last tag. Falls back to "dev" for local builds that don't define
// GIT_REVISION.
#ifdef GIT_REVISION
#define N02_VERSION N02_TOSTRING(GIT_REVISION)
#else
#define N02_VERSION "dev"
#endif

#define N02_WINDOW_TITLE "N02 " N02_VERSION
