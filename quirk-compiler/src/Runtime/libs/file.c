#include "../types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Automatically called when you do `f := File("data.txt", "w")`
void Io_File_File___init(File* self, String* path, String* mode) {
    if (!self) return;
    
    char* path_str = (path && path->buffer) ? path->buffer : "";
    char* mode_str = (mode && mode->buffer) ? mode->buffer : "r";

    self->handle = fopen(path_str, mode_str);
    self->is_open = (self->handle != NULL) ? 1 : 0;
}

void Io_File_File_close(File* self) {
    if (self && self->is_open && self->handle) {
        // Prevent closing standard streams (stdout/stderr/stdin)
        if (self->handle != stdout && self->handle != stderr && self->handle != stdin) {
            fclose((FILE*)self->handle);
        }
        self->handle = NULL;
        self->is_open = 0;
    }
}

String* Io_File_File_read_line(File* self) {
    if (!self || !self->is_open || !self->handle) return make_String("");
    
    char buffer[2048]; // Supports lines up to 2048 chars
    if (fgets(buffer, sizeof(buffer), (FILE*)self->handle)) {
        // Trim the trailing newline
        buffer[strcspn(buffer, "\n")] = 0;
        return make_String(buffer);
    }
    return make_String("");
}

String* Io_File_File_read(File* self) {
    if (!self || !self->is_open || !self->handle) return make_String("");

    fseek((FILE*)self->handle, 0, SEEK_END);
    long length = ftell((FILE*)self->handle);
    fseek((FILE*)self->handle, 0, SEEK_SET);

    if (length <= 0) return make_String("");

    char* buf = malloc(length + 1);
    if (!buf) return make_String("");

    size_t read_bytes = fread(buf, 1, length, (FILE*)self->handle);
    buf[read_bytes] = '\0';

    return make_String_taking_ownership(buf);
}

void Io_File_File_write(File* self, String* s) {
    if (!self || !self->is_open || !self->handle || !s || !s->buffer) return;

    // We use s->length instead of strlen(s->buffer) for better performance 
    // and safety in case of null-bytes inside the string!
    fwrite(s->buffer, 1, s->length, (FILE*)self->handle);
    
    // Flush to ensure it hits the disk/console immediately
    fflush((FILE*)self->handle); 
}