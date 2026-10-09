#pragma once

// Host previews do not receive VideoOut events, but renderer.cpp still needs
// the opaque queue type in its platform declarations.
#if defined(BOTTY_HOST_PREVIEW) && defined(__linux__)
struct kevent {};
#else
#include <sys/event.h>
#endif
