#pragma once
// Single source of the product version. Shared by C++ code and resources/app.rc,
// so it must stay plain preprocessor definitions.
#define TOKENSCOPE_VERSION_MAJOR 0
#define TOKENSCOPE_VERSION_MINOR 5
#define TOKENSCOPE_VERSION_PATCH 0

#define TOKENSCOPE_STRINGIFY_(value) #value
#define TOKENSCOPE_STRINGIFY(value) TOKENSCOPE_STRINGIFY_(value)
#define TOKENSCOPE_VERSION_STRING                                                                                      \
    TOKENSCOPE_STRINGIFY(TOKENSCOPE_VERSION_MAJOR)                                                                     \
    "." TOKENSCOPE_STRINGIFY(TOKENSCOPE_VERSION_MINOR) "." TOKENSCOPE_STRINGIFY(TOKENSCOPE_VERSION_PATCH)
#define TOKENSCOPE_WIDEN_(text) L##text
#define TOKENSCOPE_WIDEN(text) TOKENSCOPE_WIDEN_(text)
#define TOKENSCOPE_VERSION_WSTRING TOKENSCOPE_WIDEN(TOKENSCOPE_VERSION_STRING)
