/* Right-to-left text for the window runtime (included by window.c).
 *
 * Two steps turn a line of text (code points, logical order) into what's
 * drawn left to right:
 *   1. Arabic joining (qshape_arabic): each Arabic / Persian letter takes
 *      its isolated, initial, medial or final form — the Unicode
 *      presentation forms (U+FB50..U+FEFF), which common fonts carry —
 *      and lam + alef become one ligature. A form no font has stays the
 *      plain letter.
 *   2. Bidirectional order (qbidi_levels / qbidi_reorder): the Unicode
 *      Bidirectional Algorithm without explicit embeddings / isolates:
 *      paragraph direction from the first strong character (P2-P3), weak
 *      types W1-W7, neutrals N1-N2, levels I1-I2, trailing white space
 *      L1, reordering L2 and mirrored brackets L4.
 * No text shaping engine (HarfBuzz) is involved: scripts that need more
 * than joining forms (Indic conjuncts, mark positioning) are drawn
 * glyph by glyph as before.
 */
#ifndef QUIRK_WINDOW_BIDI_H
#define QUIRK_WINDOW_BIDI_H

enum { QB_L, QB_R, QB_AL, QB_EN, QB_AN, QB_ES, QB_ET, QB_CS, QB_NSM, QB_WS, QB_S, QB_ON };

static int qbidi_class(int c) {
    if (c < 0x80) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return QB_L;
        if (c >= '0' && c <= '9') return QB_EN;
        if (c == ' ') return QB_WS;
        if (c == '\t') return QB_S;
        if (c == '+' || c == '-') return QB_ES;
        if (c == '#' || c == '$' || c == '%') return QB_ET;
        if (c == ',' || c == '.' || c == '/' || c == ':') return QB_CS;
        if (c < 0x20) return QB_ON;
        return QB_ON;
    }
    if (c == 0xA0) return QB_CS;
    if (c >= 0xA2 && c <= 0xA5) return QB_ET;
    if (c == 0xB0 || c == 0xB1) return QB_ET;
    if (c == 0xB2 || c == 0xB3 || c == 0xB9) return QB_EN;
    if (c >= 0xA1 && c <= 0xBF) return c == 0xAA || c == 0xB5 || c == 0xBA ? QB_L : QB_ON;
    if (c == 0xD7 || c == 0xF7) return QB_ON;
    if (c >= 0x0300 && c <= 0x036F) return QB_NSM;
    if (c >= 0x0591 && c <= 0x05C7) {
        if (c == 0x05BE || c == 0x05C0 || c == 0x05C3 || c == 0x05C6) return QB_R;
        return QB_NSM;
    }
    if (c >= 0x0590 && c <= 0x05FF) return QB_R;                 /* Hebrew */
    if (c >= 0x0600 && c <= 0x06FF) {                            /* Arabic */
        if (c >= 0x0660 && c <= 0x0669) return QB_AN;
        if (c == 0x066B || c == 0x066C) return QB_AN;
        if (c == 0x066A) return QB_ET;
        if (c >= 0x06F0 && c <= 0x06F9) return QB_EN;
        if (c == 0x060C) return QB_CS;
        if ((c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||
            (c >= 0x06D6 && c <= 0x06DC) || (c >= 0x06DF && c <= 0x06E4) ||
            (c >= 0x06E7 && c <= 0x06E8) || (c >= 0x06EA && c <= 0x06ED)) return QB_NSM;
        return QB_AL;
    }
    if (c >= 0x0700 && c <= 0x08FF) return c >= 0x07C0 && c <= 0x085F ? QB_R : QB_AL;
    if (c >= 0x2000 && c <= 0x200A) return QB_WS;
    if (c == 0x200E) return QB_L;                                /* LRM */
    if (c == 0x200F) return QB_R;                                /* RLM */
    if (c == 0x061C) return QB_AL;                               /* ALM */
    if (c >= 0x2010 && c <= 0x2027) return QB_ON;
    if (c >= 0x2030 && c <= 0x2034) return QB_ET;
    if (c >= 0x2035 && c <= 0x205E) return QB_ON;
    if (c >= 0x20A0 && c <= 0x20CF) return QB_ET;
    if (c >= 0x2190 && c <= 0x2BFF) return QB_ON;                /* arrows, maths, shapes */
    if (c == 0x3000) return QB_WS;
    if (c >= 0x3001 && c <= 0x3003) return QB_ON;
    if (c >= 0xFB1D && c <= 0xFB4F) return c == 0xFB1E ? QB_NSM : QB_R;
    if (c >= 0xFB50 && c <= 0xFDFF) return QB_AL;
    if (c >= 0xFE70 && c <= 0xFEFF) return c == 0xFEFF ? QB_ON : QB_AL;
    if (c >= 0xFF10 && c <= 0xFF19) return QB_EN;
    return QB_L;
}

/* Does the line need the bidi / shaping path at all? */
static int qbidi_needed(const int* cp, int n) {
    for (int i = 0; i < n; i++)
        if (cp[i] >= 0x0590 && (cp[i] <= 0x08FF || (cp[i] >= 0xFB1D && cp[i] <= 0xFEFC) || cp[i] == 0x200F))
            return 1;
    return 0;
}

/* ---- Arabic joining ---- */

/* Presentation forms: isolated, final, initial, medial (0 = none). */
typedef struct { int cp; int f[4]; } QArForm;
static const QArForm qar_forms[] = {
    {0x0621, {0xFE80, 0, 0, 0}},
    {0x0622, {0xFE81, 0xFE82, 0, 0}}, {0x0623, {0xFE83, 0xFE84, 0, 0}},
    {0x0624, {0xFE85, 0xFE86, 0, 0}}, {0x0625, {0xFE87, 0xFE88, 0, 0}},
    {0x0626, {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}}, {0x0627, {0xFE8D, 0xFE8E, 0, 0}},
    {0x0628, {0xFE8F, 0xFE90, 0xFE91, 0xFE92}}, {0x0629, {0xFE93, 0xFE94, 0, 0}},
    {0x062A, {0xFE95, 0xFE96, 0xFE97, 0xFE98}}, {0x062B, {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}},
    {0x062C, {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}}, {0x062D, {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}},
    {0x062E, {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}}, {0x062F, {0xFEA9, 0xFEAA, 0, 0}},
    {0x0630, {0xFEAB, 0xFEAC, 0, 0}}, {0x0631, {0xFEAD, 0xFEAE, 0, 0}},
    {0x0632, {0xFEAF, 0xFEB0, 0, 0}}, {0x0633, {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}},
    {0x0634, {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}}, {0x0635, {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}},
    {0x0636, {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}}, {0x0637, {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}},
    {0x0638, {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}}, {0x0639, {0xFEC9, 0xFECA, 0xFECB, 0xFECC}},
    {0x063A, {0xFECD, 0xFECE, 0xFECF, 0xFED0}}, {0x0641, {0xFED1, 0xFED2, 0xFED3, 0xFED4}},
    {0x0642, {0xFED5, 0xFED6, 0xFED7, 0xFED8}}, {0x0643, {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}},
    {0x0644, {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}}, {0x0645, {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}},
    {0x0646, {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}}, {0x0647, {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}},
    {0x0648, {0xFEED, 0xFEEE, 0, 0}}, {0x0649, {0xFEEF, 0xFEF0, 0, 0}},
    {0x064A, {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}},
    {0x067E, {0xFB56, 0xFB57, 0xFB58, 0xFB59}}, {0x0686, {0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D}},
    {0x0698, {0xFB8A, 0xFB8B, 0, 0}}, {0x06A9, {0xFB8E, 0xFB8F, 0xFB90, 0xFB91}},
    {0x06AF, {0xFB92, 0xFB93, 0xFB94, 0xFB95}}, {0x06CC, {0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF}},
};

static const QArForm* qar_find(int c) {
    int lo = 0, hi = (int)(sizeof qar_forms / sizeof qar_forms[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (qar_forms[mid].cp == c) return &qar_forms[mid];
        if (qar_forms[mid].cp < c) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static int qar_transparent(int c) { return qbidi_class(c) == QB_NSM && c >= 0x0600 && c <= 0x06FF; }
/* Joins to the following letter (dual-joining, or tatweel). */
static int qar_joins_next(int c) {
    if (c == 0x0640) return 1;
    const QArForm* f = qar_find(c);
    return f && f->f[2] != 0;
}
/* Joins to the preceding letter (dual- or right-joining, or tatweel). */
static int qar_joins_prev(int c) {
    if (c == 0x0640) return 1;
    const QArForm* f = qar_find(c);
    return f && f->f[1] != 0;
}

/* Shape `n` code points (logical order) into `out` (at most n entries);
 * `cstart[k]` / `clen[k]` = the logical characters glyph k stands for
 * (2 for a lam-alef ligature). `has(c, ctx)` says whether a font can draw
 * c. Returns the glyph count. */
static int qshape_arabic(const int* cp, int n, int* out, int* cstart, int* clen,
                         int (*has)(int, void*), void* ctx) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        int c = cp[i];
        const QArForm* f = qar_find(c);
        if (!f) { out[k] = c; cstart[k] = i; clen[k] = 1; k++; continue; }
        /* Neighbours, skipping transparent marks. */
        int p = i - 1; while (p >= 0 && qar_transparent(cp[p])) p--;
        int q = i + 1; while (q < n && qar_transparent(cp[q])) q++;
        int joinPrev = p >= 0 && qar_joins_next(cp[p]) && qar_joins_prev(c);
        /* lam + alef → one ligature glyph */
        if (c == 0x0644 && q < n && (cp[q] == 0x0622 || cp[q] == 0x0623 || cp[q] == 0x0625 || cp[q] == 0x0627)) {
            int base = cp[q] == 0x0622 ? 0xFEF5 : cp[q] == 0x0623 ? 0xFEF7 : cp[q] == 0x0625 ? 0xFEF9 : 0xFEFB;
            int lig = base + (joinPrev ? 1 : 0);
            if (has(lig, ctx)) {
                out[k] = lig; cstart[k] = i; clen[k] = q - i + 1; k++;
                /* marks between lam and alef are dropped into the ligature */
                i = q;
                continue;
            }
        }
        int joinNext = q < n && qar_joins_next(c) && qar_joins_prev(cp[q]);
        int form = joinPrev ? (joinNext ? 3 : 1) : (joinNext ? 2 : 0);
        int g = f->f[form];
        if (!g && form == 3) g = f->f[1];        /* no medial: final */
        if (!g && form == 2) g = f->f[0];        /* no initial: isolated */
        if (!g || !has(g, ctx)) g = c;
        out[k] = g; cstart[k] = i; clen[k] = 1; k++;
    }
    return k;
}

/* ---- bidi levels ---- */

/* Resolve the embedding level of each of the `n` glyphs whose original
 * character classes are `cls` (modified in place). `base` = 0 / 1 forces
 * the paragraph direction, -1 picks it from the first strong character.
 * Returns the paragraph level. */
static int qbidi_levels(int* cls, int n, int base, unsigned char* level) {
    if (base < 0) {
        base = 0;
        for (int i = 0; i < n; i++) {
            if (cls[i] == QB_L) { base = 0; break; }
            if (cls[i] == QB_R || cls[i] == QB_AL) { base = 1; break; }
        }
    }
    int sor = base ? QB_R : QB_L;
    /* W1: NSM takes the previous type. */
    for (int i = 0; i < n; i++)
        if (cls[i] == QB_NSM) cls[i] = i == 0 ? sor : cls[i - 1];
    /* W2: EN after AL → AN.  W3: AL → R. */
    int last = sor;
    for (int i = 0; i < n; i++) {
        if (cls[i] == QB_L || cls[i] == QB_R || cls[i] == QB_AL) last = cls[i];
        if (cls[i] == QB_EN && last == QB_AL) cls[i] = QB_AN;
    }
    for (int i = 0; i < n; i++) if (cls[i] == QB_AL) cls[i] = QB_R;
    /* W4: a single separator between two numbers of the same kind. */
    for (int i = 1; i + 1 < n; i++) {
        if (cls[i] == QB_ES && cls[i - 1] == QB_EN && cls[i + 1] == QB_EN) cls[i] = QB_EN;
        else if (cls[i] == QB_CS && cls[i - 1] == QB_EN && cls[i + 1] == QB_EN) cls[i] = QB_EN;
        else if (cls[i] == QB_CS && cls[i - 1] == QB_AN && cls[i + 1] == QB_AN) cls[i] = QB_AN;
    }
    /* W5: terminators next to European numbers. */
    for (int i = 0; i < n; i++) {
        if (cls[i] != QB_ET) continue;
        int j = i; while (j < n && cls[j] == QB_ET) j++;
        if ((i > 0 && cls[i - 1] == QB_EN) || (j < n && cls[j] == QB_EN))
            for (int k = i; k < j; k++) cls[k] = QB_EN;
        i = j - 1;
    }
    /* W6: remaining separators / terminators are neutral. */
    for (int i = 0; i < n; i++)
        if (cls[i] == QB_ES || cls[i] == QB_ET || cls[i] == QB_CS) cls[i] = QB_ON;
    /* W7: EN after L (or at an L start) → L. */
    last = sor;
    for (int i = 0; i < n; i++) {
        if (cls[i] == QB_L || cls[i] == QB_R) last = cls[i];
        if (cls[i] == QB_EN && last == QB_L) cls[i] = QB_L;
    }
    /* N1 / N2: neutrals between strong types of the same direction take it
     * (numbers count as R); otherwise the paragraph direction. */
    for (int i = 0; i < n; i++) {
        if (!(cls[i] == QB_ON || cls[i] == QB_WS || cls[i] == QB_S)) continue;
        int j = i; while (j < n && (cls[j] == QB_ON || cls[j] == QB_WS || cls[j] == QB_S)) j++;
        int before = i == 0 ? sor : (cls[i - 1] == QB_L ? QB_L : QB_R);
        int after = j >= n ? sor : (cls[j] == QB_L ? QB_L : QB_R);
        int set = before == after ? before : (base ? QB_R : QB_L);
        for (int k = i; k < j; k++) cls[k] = set;
        i = j - 1;
    }
    /* I1 / I2 */
    for (int i = 0; i < n; i++) {
        int t = cls[i];
        if (base == 0) level[i] = t == QB_R ? 1 : (t == QB_AN || t == QB_EN) ? 2 : 0;
        else level[i] = (t == QB_L || t == QB_EN || t == QB_AN) ? 2 : 1;
    }
    return base;
}

/* L2: the visual order of `n` glyphs as indices into the logical array. */
static void qbidi_reorder(const unsigned char* level, int n, int* order) {
    int maxl = 0, minodd = 255;
    for (int i = 0; i < n; i++) {
        order[i] = i;
        if (level[i] > maxl) maxl = level[i];
        if ((level[i] & 1) && level[i] < minodd) minodd = level[i];
    }
    for (int l = maxl; l >= minodd && l > 0; l--) {
        for (int i = 0; i < n; i++) {
            if (level[order[i]] < l) continue;
            int j = i; while (j < n && level[order[j]] >= l) j++;
            for (int a = i, b = j - 1; a < b; a++, b--) { int t = order[a]; order[a] = order[b]; order[b] = t; }
            i = j;
        }
    }
}

/* L4: a bracket in a right-to-left run is drawn mirrored. */
static int qbidi_mirror(int c) {
    switch (c) {
        case '(': return ')'; case ')': return '(';
        case '[': return ']'; case ']': return '[';
        case '{': return '}'; case '}': return '{';
        case '<': return '>'; case '>': return '<';
        case 0xAB: return 0xBB; case 0xBB: return 0xAB;
        case 0x2039: return 0x203A; case 0x203A: return 0x2039;
        default: return c;
    }
}

#endif
