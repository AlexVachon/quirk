#include "../types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

extern void quirk_throw_exception(const char* type_name, const char* message);

// ==========================================
//  INTEGER METHODS
// ==========================================

// Converts raw int -> String Object
String* Core_Primitives_Int_str(int self) {
    char buffer[64];
    snprintf(buffer, 64, "%d", self);
    return make_String(buffer);
}

// Converts raw int -> raw double
double Core_Primitives_Int_to_float(int self) {
    return (double)self;
}

int Core_Primitives_Int_abs(int self) {
    return (self < 0) ? -self : self;
}

int Core_Primitives_Int_pow(int self, int exp) {
    // pow() takes doubles, so we cast
    return (int)pow((double)self, (double)exp);
}

int Core_Primitives_Int_is_even(int self) {
    return (self % 2) == 0;
}

int Core_Primitives_Int_is_odd(int self) {
    return (self % 2) != 0;
}

// ==========================================
//  DOUBLE METHODS
// ==========================================

// Converts raw double -> String Object
String* Core_Primitives_Double_str(double self) {
    char buffer[64];
    // %g automatically removes trailing zeros
    snprintf(buffer, 64, "%g", self);
    return make_String(buffer);
}

// Converts raw double -> raw int (Truncates)
int Core_Primitives_Double_to_int(double self) {
    return (int)self;
}

double Core_Primitives_Double_abs(double self) {
    return fabs(self);
}

double Core_Primitives_Double_ceil(double self) {
    return ceil(self);
}

double Core_Primitives_Double_floor(double self) {
    return floor(self);
}

double Core_Primitives_Double_round(double self) {
    return round(self);
}

double Core_Primitives_Double_sqrt(double self) {
    return sqrt(self);
}

String* Core_Primitives_Bool_str(int self) {
    return make_String(self ? "true" : "false");
}

String* Core_Primitives_Char_str(char self) {
    char buffer[2] = {self, '\0'};
    return make_String(buffer);
}

char Core_Primitives_Char___init() { return '\0'; }
int Core_Primitives_Char_is_upper(char self) { return isupper((unsigned char)self) != 0; }
int Core_Primitives_Char_is_lower(char self) { return islower((unsigned char)self) != 0; }
int Core_Primitives_Char_is_digit(char self) { return isdigit((unsigned char)self) != 0; }
int Core_Primitives_Char_is_alpha(char self) { return isalpha((unsigned char)self) != 0; }
int Core_Primitives_Char_is_space(char self) { return isspace((unsigned char)self) != 0; }

char Core_Primitives_Char_to_upper(char self) { return toupper((unsigned char)self); }
char Core_Primitives_Char_to_lower(char self) { return tolower((unsigned char)self); }

// ==========================================
//  PARSE METHODS (static — no self)
// ==========================================

int Core_Primitives_Int_parse(String* s) {
    if (!s || !s->buffer || s->length == 0) {
        quirk_throw_exception("ValueError", "cannot parse empty string as Int");
        return 0;
    }
    char* end;
    long val = strtol(s->buffer, &end, 10);
    if (end == s->buffer || *end != '\0') {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid literal for Int: '%s'", s->buffer);
        quirk_throw_exception("ValueError", msg);
        return 0;
    }
    return (int)val;
}

double Core_Primitives_Double_parse(String* s) {
    if (!s || !s->buffer || s->length == 0) {
        quirk_throw_exception("ValueError", "cannot parse empty string as Double");
        return 0.0;
    }
    char* end;
    double val = strtod(s->buffer, &end);
    if (end == s->buffer || *end != '\0') {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid literal for Double: '%s'", s->buffer);
        quirk_throw_exception("ValueError", msg);
        return 0.0;
    }
    return val;
}

int8_t Core_Primitives_Bool_parse(String* s) {
    if (!s || !s->buffer) {
        quirk_throw_exception("ValueError", "cannot parse null as Bool");
        return 0;
    }
    if (strcmp(s->buffer, "true") == 0)  return 1;
    if (strcmp(s->buffer, "false") == 0) return 0;
    char msg[256];
    snprintf(msg, sizeof(msg),
             "invalid literal for Bool: '%s' (expected 'true' or 'false')", s->buffer);
    quirk_throw_exception("ValueError", msg);
    return 0;
}

char Core_Primitives_Char_parse(String* s) {
    if (!s || !s->buffer || s->length == 0) {
        quirk_throw_exception("ValueError", "cannot parse empty string as Char");
        return '\0';
    }
    if (s->length != 1) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "invalid literal for Char: expected single character, got '%s'", s->buffer);
        quirk_throw_exception("ValueError", msg);
        return '\0';
    }
    return s->buffer[0];
}

String* Core_Primitives_Int___str(int self)    { return Core_Primitives_Int_str(self); }
String* Core_Primitives_Double___str(double self) { return Core_Primitives_Double_str(self); }
String* Core_Primitives_Bool___str(int self)   { return Core_Primitives_Bool_str(self); }
String* Core_Primitives_Char___str(char self)  { return Core_Primitives_Char_str(self); }