#include "../../types.h"
#include <stdint.h>

extern void* GC_malloc(size_t);

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String* Base64_encode(String* input) {
    if (!input || !input->buffer || input->length == 0) return make_String("");
    
    size_t in_len = input->length;
    size_t out_len = 4 * ((in_len + 2) / 3);
    char* out = (char*)GC_malloc(out_len + 1);
    
    for (size_t i = 0, j = 0; i < in_len;) {
        uint32_t octet_a = i < in_len ? (unsigned char)input->buffer[i++] : 0;
        uint32_t octet_b = i < in_len ? (unsigned char)input->buffer[i++] : 0;
        uint32_t octet_c = i < in_len ? (unsigned char)input->buffer[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        out[j++] = base64_table[(triple >> 3 * 6) & 0x3F];
        out[j++] = base64_table[(triple >> 2 * 6) & 0x3F];
        out[j++] = base64_table[(triple >> 1 * 6) & 0x3F];
        out[j++] = base64_table[(triple >> 0 * 6) & 0x3F];
    }

    for (size_t i = 0; i < (3 - in_len % 3) % 3; i++) {
        out[out_len - 1 - i] = '=';
    }
    out[out_len] = '\0';
    
    // We manually construct the String object to ensure it is Binary Safe
    String* res = (String*)GC_malloc(sizeof(String));
    res->buffer = out;
    res->length = out_len;
    return res;
}

static int b64_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

String* Base64_decode(String* input) {
    if (!input || !input->buffer || input->length == 0) return make_String("");
    
    size_t in_len = input->length;
    if (in_len % 4 != 0) return make_String(""); 
    
    size_t out_len = in_len / 4 * 3;
    if (input->buffer[in_len - 1] == '=') out_len--;
    if (input->buffer[in_len - 2] == '=') out_len--;
    
    char* out = (char*)GC_malloc(out_len + 1);
    size_t j = 0;
    
    for (size_t i = 0; i < in_len;) {
        int a = input->buffer[i] == '=' ? 0 & i++ : b64_index(input->buffer[i++]);
        int b = input->buffer[i] == '=' ? 0 & i++ : b64_index(input->buffer[i++]);
        int c = input->buffer[i] == '=' ? 0 & i++ : b64_index(input->buffer[i++]);
        int d = input->buffer[i] == '=' ? 0 & i++ : b64_index(input->buffer[i++]);
        
        uint32_t triple = (a << 3 * 6) + (b << 2 * 6) + (c << 1 * 6) + d;
        
        if (j < out_len) out[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < out_len) out[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < out_len) out[j++] = (triple >> 0 * 8) & 0xFF;
    }
    out[out_len] = '\0';
    
    String* res = (String*)GC_malloc(sizeof(String));
    res->buffer = out;
    res->length = out_len;
    return res;
}