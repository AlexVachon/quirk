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
String* Core_Primitives_Int_str(int64_t self) {
    char buffer[64];
    snprintf(buffer, 64, "%lld", (long long)self);
    return make_String(buffer);
}

// Converts raw int -> raw double
double Core_Primitives_Int_to_float(int64_t self) {
    return (double)self;
}

int64_t Core_Primitives_Int_abs(int64_t self) {
    return (self < 0) ? -self : self;
}

int64_t Core_Primitives_Int_pow(int64_t self, int64_t exp) {
    // pow() takes doubles, so we cast
    return (int)pow((double)self, (double)exp);
}

int Core_Primitives_Int_is_even(int64_t self) {
    return (self % 2) == 0;
}

int Core_Primitives_Int_is_odd(int64_t self) {
    return (self % 2) != 0;
}

// ==========================================
//  DOUBLE METHODS
// ==========================================

void quirk_format_double(double v, char* out, size_t n) {
    if (isnan(v)) { snprintf(out, n, "nan"); return; }
    if (isinf(v)) { snprintf(out, n, v < 0 ? "-inf" : "inf"); return; }
    // Shortest text that round-trips (%g alone keeps 6 significant
    // digits: 3.14159265 → "3.14159", 1234567.5 → "1.23457e+06"). Plain
    // decimals for 1e-4 <= |v| < 1e16, an exponent outside — as Python.
    double a = fabs(v);
    if (a == 0.0 || (a >= 1e-4 && a < 1e16)) {
        for (int d = 0; d <= 20; d++) {
            snprintf(out, n, "%.*f", d, v);
            if (strtod(out, NULL) == v) break;
        }
    } else {
        for (int prec = 1; prec <= 17; prec++) {
            snprintf(out, n, "%.*g", prec, v);
            if (strtod(out, NULL) == v) break;
        }
    }
    // Whole numbers keep a ".0" so a Double never reads as an Int.
    if (!strpbrk(out, ".eEn")) strncat(out, ".0", n - strlen(out) - 1);
}

void quirk_print_double(double v, int32_t newline) {
    char buf[64];
    quirk_format_double(v, buf, sizeof(buf));
    fputs(buf, stdout);
    if (newline) fputc('\n', stdout);
}

// Converts raw double -> String Object
String* Core_Primitives_Double_str(double self) {
    char buffer[64];
    quirk_format_double(self, buffer, sizeof(buffer));
    return make_String(buffer);
}

// Converts raw double -> raw int (Truncates)
int64_t Core_Primitives_Double_to_int(double self) {
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

char Core_Primitives_Char_upper(char self) { return toupper((unsigned char)self); }
char Core_Primitives_Char_lower(char self) { return tolower((unsigned char)self); }

// ==========================================
//  PARSE METHODS (static — no self)
// ==========================================

int64_t Core_Primitives_Int_parse(String* s) {
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

String* Core_Primitives_Int___str(int64_t self)    { return Core_Primitives_Int_str(self); }
String* Core_Primitives_Double___str(double self) { return Core_Primitives_Double_str(self); }
String* Core_Primitives_Bool___str(int self)   { return Core_Primitives_Bool_str(self); }
String* Core_Primitives_Char___str(char self)  { return Core_Primitives_Char_str(self); }