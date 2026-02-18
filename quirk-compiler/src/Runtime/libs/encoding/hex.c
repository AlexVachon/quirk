#include "../../types.h"

extern void* GC_malloc(size_t);

String* Hex_encode(String* input) {
    if (!input || !input->buffer || input->length == 0) return make_String("");
    
    size_t in_len = input->length;
    char* out = (char*)GC_malloc(in_len * 2 + 1);
    const char* hex_chars = "0123456789abcdef";
    
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2]     = hex_chars[((unsigned char)input->buffer[i]) >> 4];
        out[i * 2 + 1] = hex_chars[((unsigned char)input->buffer[i]) & 0x0F];
    }
    out[in_len * 2] = '\0';
    
    String* res = (String*)GC_malloc(sizeof(String));
    res->buffer = out;
    res->length = in_len * 2;
    return res;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

String* Hex_decode(String* input) {
    if (!input || !input->buffer || input->length == 0) return make_String("");
    
    size_t in_len = input->length;
    if (in_len % 2 != 0) return make_String(""); // Hex strings must be even length
    
    size_t out_len = in_len / 2;
    char* out = (char*)GC_malloc(out_len + 1);
    
    for (size_t i = 0; i < out_len; i++) {
        out[i] = (hex_val(input->buffer[i * 2]) << 4) | hex_val(input->buffer[i * 2 + 1]);
    }
    out[out_len] = '\0';
    
    String* res = (String*)GC_malloc(sizeof(String));
    res->buffer = out;
    res->length = out_len;
    return res;
}