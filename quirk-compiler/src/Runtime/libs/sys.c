#include <stdlib.h>
#include <string.h>

static int quirk_argc = 0;
static char** quirk_argv = NULL;

// Called by LLVM at the very beginning of the program
void Sys_init(int argc, char** argv) {
    quirk_argc = argc;
    quirk_argv = argv;
}

int Sys_arg_count() {
    return quirk_argc;
}

char* Sys_arg_get(int index) {
    if (index < 0 || index >= quirk_argc) return "";
    return quirk_argv[index];
}

char* Sys_getenv(char* name) {
    char* val = getenv(name);
    return val ? val : "";
}

void Sys_exit(int code) {
    exit(code);
}

int Sys_system(char* command) {
    return system(command);
}

// Add this to the bottom of src/Runtime/libs/sys.c

char* Sys_srcline(const char* filename, int target_line) {
    FILE* f = fopen(filename, "r");
    if (!f) return strdup("");
    
    char buffer[1024];
    int current = 1;
    while (fgets(buffer, sizeof(buffer), f)) {
        if (current == target_line) {
            fclose(f);
            
            // Trim leading whitespace
            char* start = buffer;
            while (*start == ' ' || *start == '\t') start++;
            
            // Trim trailing newline
            size_t len = strlen(start);
            if (len > 0 && start[len-1] == '\n') start[len-1] = '\0';
            
            return strdup(start);
        }
        current++;
    }
    fclose(f);
    return strdup("");
}