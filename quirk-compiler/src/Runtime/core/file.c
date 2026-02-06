#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FIX: Accept String* instead of char*
File* File_open(String* path, String* mode) {
    File* f = (File*)malloc(sizeof(File));

    // Extract raw C strings from the Apex String objects
    char* path_str = path ? path->buffer : "";
    char* mode_str = mode ? mode->buffer : "r";

    f->handle = fopen(path_str, mode_str);

    if (f->handle) {
        f->is_open = 1;
    } else {
        f->is_open = 0;
    }
    return f;
}

void File_close(File* f) {
    if (f && f->is_open && f->handle) {
        fclose((FILE*)f->handle);
        f->handle = NULL;
        f->is_open = 0;
    }
}

// FIXED: Corrected parameter name from raw_handle to f
String* File_read_line(File* f) {
    if (!f || !f->is_open || !f->handle)  // FIXED: Was "if (!raw_handle)"
        return NULL;
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer),
              (FILE*)f->handle)) {  // FIXED: Was raw_handle
        buffer[strcspn(buffer, "\n")] = 0;
        return make_String(buffer);
    }
    return NULL;
}

String* File_read(File* f) {
    if (!f || !f->is_open || !f->handle)
        return NULL;

    fseek((FILE*)f->handle, 0, SEEK_END);
    long length = ftell((FILE*)f->handle);
    fseek((FILE*)f->handle, 0, SEEK_SET);

    char* buf = malloc(length + 1);
    fread(buf, 1, length, (FILE*)f->handle);
    buf[length] = 0;

    return make_String(buf);
}

void File_write(File* f, String* s) {
    // 1. Safety Checks (matching your read logic)
    if (!f || !f->is_open || !f->handle || !s || !s->buffer)
        return;

    // 2. Write the data
    // We use s->length because your String struct explicitly tracks it.
    // This is safer/faster than strlen, especially if you ever handle binary data.
    fwrite(s->buffer, 1, s->length, (FILE*)f->handle);
    
    // Optional: Flush to ensure data hits the disk immediately
    fflush((FILE*)f->handle); 
}