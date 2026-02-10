// The "Unity Build" approach: Include the C files directly.
// This is the simplest way to link everything into one .so without complex Makefiles.
#include "core/types.h"

#include "core/string.c"
#include "core/primitives.c"
#include "core/list.c"
#include "core/map.c"

#include "core/file.c"

// If you need global runtime initialization later, put it here.