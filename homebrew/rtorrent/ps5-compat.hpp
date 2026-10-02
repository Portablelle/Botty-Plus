#pragma once
#include <netdb.h>
#include <wchar.h>
#include <limits.h>
// The PS5 resolver exposes the POSIX error set. The obsolete extensions both
// describe the absence of an address for the requested name/family.
#ifndef EAI_ADDRFAMILY
#define EAI_ADDRFAMILY EAI_NONAME
#endif
#ifndef EAI_NODATA
#define EAI_NODATA EAI_NONAME
#endif

// The SDK has wcwidth but not wcswidth. rTorrent still links this renderer
// helper in daemon-only builds. Preserve the standard error/termination rules.
static inline int botty_wcswidth(const wchar_t* text, size_t count) {
    int total = 0;
    for (size_t i = 0; i < count && text[i]; ++i) {
        int width = wcwidth(text[i]);
        if (width < 0 || total > INT_MAX - width) return -1;
        total += width;
    }
    return total;
}
#define wcswidth botty_wcswidth
