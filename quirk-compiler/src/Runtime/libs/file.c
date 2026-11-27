#include "../types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <errno.h>
#endif

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
    // Other tasks run while this waits (stdin: console.input).
    quirk_gil_release();
    char* got = fgets(buffer, sizeof(buffer), (FILE*)self->handle);
    quirk_gil_acquire();
    if (got) {
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

    quirk_gil_release();
    size_t read_bytes = fread(buf, 1, length, (FILE*)self->handle);
    quirk_gil_acquire();
    buf[read_bytes] = '\0';

    return make_String_len(buf, (int64_t)read_bytes);   // binary-safe
}

void Io_File_File_write(File* self, String* s) {
    if (!self || !self->is_open || !self->handle || !s || !s->buffer) return;

    // We use s->length instead of strlen(s->buffer) for better performance 
    // and safety in case of null-bytes inside the string!
    fwrite(s->buffer, 1, s->length, (FILE*)self->handle);
    
    // Flush to ensure it hits the disk/console immediately
    fflush((FILE*)self->handle); 
}

// Up to `max` bytes, as soon as there are any (a pipe's, stdin's: what has
// arrived; not waiting for all `max` as fread does) — for a program that
// talks over its stdin, a language server. "" at the end (or on an error).
// Bytes, not characters: a UTF-8 character can be split between two reads.
// (It reads the descriptor itself: don't mix with read_line on the same file.)
String* Io_File_File_read_some(File* self, int64_t max) {
    if (!self || !self->is_open || !self->handle || max <= 0) return make_String("");
    if (max > (1 << 20)) max = 1 << 20;
    char* buf = malloc((size_t)max + 1);
    if (!buf) return make_String("");
    int fd = fileno((FILE*)self->handle);
    quirk_gil_release();
#ifdef _WIN32
    int n = _read(fd, buf, (unsigned)max);
#else
    ssize_t n;
    do { n = read(fd, buf, (size_t)max); } while (n < 0 && errno == EINTR);
#endif
    quirk_gil_acquire();
    if (n <= 0) {
        free(buf);
        return make_String("");
    }
    buf[n] = '\0';
    return make_String_len(buf, (int64_t)n);            // (it takes `buf` over)
}

// No newline translation (Windows' text mode turns "\n" into "\r\n" on the
// way out, and back on the way in): bytes as they are, for a protocol that
// counts them. Nothing to do elsewhere.
void Io_File_File_set_binary(File* self) {
    if (!self || !self->is_open || !self->handle) return;
#ifdef _WIN32
    fflush((FILE*)self->handle);
    _setmode(_fileno((FILE*)self->handle), _O_BINARY);
#endif
}
