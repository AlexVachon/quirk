#include "../types.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

// ==========================================
//  INTEGER METHODS
// ==========================================

// Converts raw int -> String Object
String* Int_str(int self) {
    char buffer[64];
    snprintf(buffer, 64, "%d", self);
    return make_String(buffer);
}

// Converts raw int -> raw double
double Int_to_float(int self) {
    return (double)self;
}

int Int_abs(int self) {
    return (self < 0) ? -self : self;
}

int Int_pow(int self, int exp) {
    // pow() takes doubles, so we cast
    return (int)pow((double)self, (double)exp);
}

int Int_is_even(int self) {
    return (self % 2) == 0;
}

int Int_is_odd(int self) {
    return (self % 2) != 0;
}

// ==========================================
//  DOUBLE METHODS
// ==========================================

// Converts raw double -> String Object
String* Double_str(double self) {
    char buffer[64];
    // %g automatically removes trailing zeros
    snprintf(buffer, 64, "%g", self);
    return make_String(buffer);
}

// Converts raw double -> raw int (Truncates)
int Double_to_int(double self) {
    return (int)self;
}

double Double_abs(double self) {
    return fabs(self);
}

double Double_ceil(double self) {
    return ceil(self);
}

double Double_floor(double self) {
    return floor(self);
}

double Double_round(double self) {
    return round(self);
}

double Double_sqrt(double self) {
    return sqrt(self);
}

String* Bool_str(int self) {
    return make_String(self ? "true" : "false");
}

String* Char_str(char self) {
    char buffer[2] = {self, '\0'};
    return make_String(buffer);
}

char Char__init() { return '\0'; }
int Char_is_upper(char self) { return isupper((unsigned char)self) != 0; }
int Char_is_lower(char self) { return islower((unsigned char)self) != 0; }
int Char_is_digit(char self) { return isdigit((unsigned char)self) != 0; }
int Char_is_alpha(char self) { return isalpha((unsigned char)self) != 0; }
int Char_is_space(char self) { return isspace((unsigned char)self) != 0; }

char Char_to_upper(char self) { return toupper((unsigned char)self); }
char Char_to_lower(char self) { return tolower((unsigned char)self); }
