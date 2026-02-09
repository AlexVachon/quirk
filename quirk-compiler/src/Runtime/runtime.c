// The "Unity Build" approach: Include the C files directly.
// This is the simplest way to link everything into one .so without complex Makefiles.

#include "core/string.c"
#include "core/primitives.c"
#include "core/file.c"
#include "core/list.c"

// If you need global runtime initialization later, put it here.