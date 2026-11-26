/* NetHack 3.6  rus.c -- Russian localization: declension (MacJr port) */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * See rus.h for the marks.  Strings here are CP866 at run time (the
 * source is UTF-8 and the compiler converts it), so a letter is a byte.
 *
 * ru_render() works on a token list: words (with the marks and the
 * {hint} that follow them), spaces, punctuation and case marks (@р).
 * Every head noun takes the case of the case mark in front of its
 * phrase -- the adjectives, pronoun and count right before it -- and
 * its adjectives agree with it.  A case mark with no marked head after
 * it declines the next plain phrase (adjectives + first noun), so
 * "@т%s" with body_part() gives "рукой".
 */

#ifdef RUS_TEST
#include "rustest.h"
#else
#include "hack.h"
#endif
#include "rus.h"

enum { C_NOM, C_GEN, C_DAT, C_ACC, C_INS, C_PRE };
enum { G_M, G_F, G_N, G_PL };

#define F_HEAD 0x001
#define F_ANIM 0x002
#define F_PL   0x004
#define F_FEM  0x008
#define F_NEU  0x010
#define F_MASC 0x020
#define F_PLT  0x040
#define F_INDC 0x080
#define F_ADJN 0x100
#define F_APPS 0x200

#ifndef RUS_TEST
const char *en_oc_name[NUM_OBJECTS + 1];
const char *en_oc_descr[NUM_OBJECTS + 1];
const char *en_mname[NUMMONS + 1];
#endif

/* ---------------------------------------------------------------- */
/* letters (CP866) */

boolean
ru_is_cyr(c)
int c;
{
    c &= 0xff;
    return (boolean) ((c >= 0x80 && c <= 0xaf) || (c >= 0xe0 && c <= 0xf1));
}

static int
ru_lower(c)
int c;
{
    c &= 0xff;
    if (c >= 0x80 && c <= 0x8f)
        return c + 0x20;
    if (c >= 0x90 && c <= 0x9f)
        return c + 0x50;
    if (c == 0xf0)
        return 0xf1;
    if (c >= 'A' && c <= 'Z')
        return c | 040;
    return c;
}

static int
ru_upper(c)
int c;
{
    c &= 0xff;
    if (c >= 0xa0 && c <= 0xaf)
        return c - 0x20;
    if (c >= 0xe0 && c <= 0xef)
        return c - 0x50;
    if (c == 0xf1)
        return 0xf0;
    if (c >= 'a' && c <= 'z')
        return c & ~040;
    return c;
}

static boolean
is_upper(c)
int c;
{
    c &= 0xff;
    return (boolean) ((c >= 0x80 && c <= 0x9f) || c == 0xf0
                      || (c >= 'A' && c <= 'Z'));
}

boolean
ru_has_cyr(s)
const char *s;
{
    if (s)
        for (; *s; s++)
            if (ru_is_cyr(*s))
                return TRUE;
    return FALSE;
}

/* the one-byte letters, for comparing against literals */
static const char L_VEL[] = "гкх", L_SIB[] = "жшчщ";

static boolean
in_set(c, set)
int c;
const char *set;
{
    return (boolean) (c && index(set, (char) c) != 0);
}

#define is_vel(c) in_set((c), L_VEL)
#define is_sib(c) in_set((c), L_SIB)

static boolean
is_vowel(c)
int c;
{
    return in_set(c, "аеёиоуыэюя");
}

/* ---------------------------------------------------------------- */
/* words */

/* memchr() (not exported by the MacJr firmware) */
static const void *
ru_memchr(p, c, n)
const void *p;
int c;
size_t n;
{
    const unsigned char *q = (const unsigned char *) p;

    for (; n > 0; n--, q++)
        if (*q == (unsigned char) c)
            return q;
    return 0;
}

/* lowercase copy of w[0..n) into lw, NUL-terminated */
static void
lowcopy(lw, w, n)
char *lw;
const char *w;
int n;
{
    int i;

    for (i = 0; i < n; i++)
        lw[i] = (char) ru_lower(w[i]);
    lw[n] = '\0';
}

static boolean
ends(lw, n, e)
const char *lw;
int n;
const char *e;
{
    int el = (int) strlen(e);

    return (boolean) (n >= el && !strncmp(lw + n - el, e, el));
}

static boolean
word_cyr(w, n)
const char *w;
int n;
{
    int i;

    for (i = 0; i < n; i++)
        if (ru_is_cyr(w[i]))
            return TRUE;
    return FALSE;
}

static const char *const adj_endings[] = { "ый", "ий", "ой", "ая", "яя",
                                           "ое", "ее", "ые", "ие", 0 };
static const char *const not_adj[] = { "мой", "твой", "свой", "герой",
                                       "покой", "какой", "кий", 0 };

static boolean
is_pronoun(lw)
const char *lw;
{
    return (boolean) (!strcmp(lw, "ваш") || !strcmp(lw, "наш"));
}

static boolean
is_adjlike(w, n)
const char *w;
int n;
{
    char lw[BUFSZ];
    int i;

    if (n < 3 || n >= BUFSZ || !word_cyr(w, n))
        return FALSE;
    lowcopy(lw, w, n);
    if (is_pronoun(lw))
        return TRUE;
    if (n < 4)
        return FALSE;
    for (i = 0; not_adj[i]; i++)
        if (!strcmp(lw, not_adj[i]))
            return FALSE;
    for (i = 0; adj_endings[i]; i++)
        if (ends(lw, n, adj_endings[i]))
            return TRUE;
    return FALSE;
}

static boolean
is_adverb(w, n)
const char *w;
int n;
{
    char lw[BUFSZ];

    if (n < 4 || n >= BUFSZ || !word_cyr(w, n))
        return FALSE;
    lowcopy(lw, w, n);
    if (!strcmp(lw, "очень") || !strcmp(lw, "совсем")
        || !strcmp(lw, "почти") || !strcmp(lw, "слегка"))
        return TRUE;
    return (boolean) (n >= 5 && lw[n - 1] == (char) 'о'
                      && (ends(lw, n, "но") || ends(lw, n, "ко")
                          || ends(lw, n, "ьно") || ends(lw, n, "чно")));
}

/* "несколько" and the like count as "many" */
static boolean
is_many(w, n)
const char *w;
int n;
{
    char lw[BUFSZ];

    if (n < 4 || n >= BUFSZ)
        return FALSE;
    lowcopy(lw, w, n);
    return (boolean) (!strcmp(lw, "несколько") || !strcmp(lw, "много")
                      || !strcmp(lw, "немного") || !strcmp(lw, "пара")
                      || !strcmp(lw, "куча"));
}

static boolean
is_number(w, n)
const char *w;
int n;
{
    int i;

    if (n < 1)
        return FALSE;
    for (i = 0; i < n; i++)
        if (!digit(w[i]))
            return FALSE;
    return TRUE;
}

static boolean
is_ench(w, n)
const char *w;
int n;
{
    return (boolean) (n >= 2 && (w[0] == '+' || w[0] == '-')
                      && is_number(w + 1, n - 1));
}

/* ---------------------------------------------------------------- */
/* adjectives */

enum { A_HARD, A_VEL, A_SIBU, A_SIBS, A_SOFT, A_PRON };

static const char *const adj_end[6][4][6] = {
    /* hard: красный */
    { { "ый", "ого", "ому", "ый", "ым", "ом" },
      { "ая", "ой", "ой", "ую", "ой", "ой" },
      { "ое", "ого", "ому", "ое", "ым", "ом" },
      { "ые", "ых", "ым", "ые", "ыми", "ых" } },
    /* velar: маленький, сухой */
    { { "ий", "ого", "ому", "ий", "им", "ом" },
      { "ая", "ой", "ой", "ую", "ой", "ой" },
      { "ое", "ого", "ому", "ое", "им", "ом" },
      { "ие", "их", "им", "ие", "ими", "их" } },
    /* sibilant, unstressed: горячий */
    { { "ий", "его", "ему", "ий", "им", "ем" },
      { "ая", "ей", "ей", "ую", "ей", "ей" },
      { "ее", "его", "ему", "ее", "им", "ем" },
      { "ие", "их", "им", "ие", "ими", "их" } },
    /* sibilant, stressed: большой */
    { { "ой", "ого", "ому", "ой", "им", "ом" },
      { "ая", "ой", "ой", "ую", "ой", "ой" },
      { "ое", "ого", "ому", "ое", "им", "ом" },
      { "ие", "их", "им", "ие", "ими", "их" } },
    /* soft: синий */
    { { "ий", "его", "ему", "ий", "им", "ем" },
      { "яя", "ей", "ей", "юю", "ей", "ей" },
      { "ее", "его", "ему", "ее", "им", "ем" },
      { "ие", "их", "им", "ие", "ими", "их" } },
    /* pronoun: ваш */
    { { "", "его", "ему", "", "им", "ем" },
      { "а", "ей", "ей", "у", "ей", "ей" },
      { "е", "его", "ему", "е", "им", "ем" },
      { "и", "их", "им", "и", "ими", "их" } },
};

/* gender a substantivized adjective has by itself (гончая, дозорный) */
static int
adj_gender(w, n)
const char *w;
int n;
{
    char lw[BUFSZ];

    if (n < 2 || n >= BUFSZ)
        return G_M;
    lowcopy(lw, w, n);
    if (ends(lw, n, "ая") || ends(lw, n, "яя"))
        return G_F;
    if (ends(lw, n, "ое") || ends(lw, n, "ее"))
        return G_N;
    if (ends(lw, n, "ые") || ends(lw, n, "ие"))
        return G_PL;
    return G_M;
}

/* decline adjective w (any nominative form) to gender g, case cs */
static void
adj_form(w, n, g, cs, anim, out)
const char *w;
int n, g, cs;
boolean anim;
char *out;
{
    char lw[BUFSZ];
    int type, stemlen, c;
    boolean masc_in;
    const char *e;

    if (n >= BUFSZ - 8 || !word_cyr(w, n)) {
        Strcpy(out, "");
        (void) strncat(out, w, n);
        return;
    }
    lowcopy(lw, w, n);
    if (is_pronoun(lw)) {
        type = A_PRON;
        stemlen = n;
        masc_in = TRUE;
    } else {
        stemlen = n - 2;
        c = stemlen > 0 ? (unsigned char) lw[stemlen - 1] : 0;
        masc_in = FALSE;
        if (ends(lw, n, "ый")) {
            type = A_HARD, masc_in = TRUE;
        } else if (ends(lw, n, "ой")) {
            type = is_vel(c) ? A_VEL : is_sib(c) ? A_SIBS : A_HARD;
            masc_in = TRUE;
        } else if (ends(lw, n, "ий")) {
            type = is_vel(c) ? A_VEL : is_sib(c) ? A_SIBU : A_SOFT;
            masc_in = TRUE;
        } else if (ends(lw, n, "ая") || ends(lw, n, "ое")) {
            type = is_vel(c) ? A_VEL : is_sib(c) ? A_SIBU : A_HARD;
        } else if (ends(lw, n, "яя")) {
            type = A_SOFT;
        } else if (ends(lw, n, "ее")) {
            type = is_sib(c) ? A_SIBU : A_SOFT;
        } else if (ends(lw, n, "ые")) {
            type = A_HARD;
        } else if (ends(lw, n, "ие")) {
            type = is_vel(c) ? A_VEL : is_sib(c) ? A_SIBU : A_SOFT;
        } else {
            Strcpy(out, "");
            (void) strncat(out, w, n);
            return;
        }
    }
    /* animate accusative: masculine and plural take the genitive */
    if (cs == C_ACC && anim && (g == G_M || g == G_PL))
        cs = C_GEN;
    Strcpy(out, "");
    (void) strncat(out, w, stemlen);
    if (g == G_M && (cs == C_NOM || cs == C_ACC) && masc_in) {
        (void) strncat(out, w + stemlen, n - stemlen); /* as written */
        return;
    }
    e = adj_end[type][g][cs];
    Strcat(out, e);
}

/* ---------------------------------------------------------------- */
/* nouns */

enum { P_MHARD, P_MSOFT, P_J, P_A, P_JA, P_FSOFT, P_O, P_E, P_IE, P_JE,
       P_PLT };

/* split hint "a;b;c" into fields */
static int
split_hint(ann, annl, f, flen)
const char *ann;
int annl;
const char *f[12];
int flen[12];
{
    int k = 0, i, start = 0;

    if (!ann || annl <= 0)
        return 0;
    for (i = 0; i <= annl && k < 12; i++) {
        if (i == annl || ann[i] == ';') {
            f[k] = ann + start;
            flen[k] = i - start;
            k++;
            start = i + 1;
        }
    }
    return k;
}

static int
noun_gender(lw, n, fl)
const char *lw;
int n;
unsigned fl;
{
    int c;

    if (fl & F_PLT)
        return G_PL;
    if (fl & F_FEM)
        return G_F;
    if (fl & F_NEU)
        return G_N;
    if (fl & F_MASC)
        return G_M;
    if (n < 1)
        return G_M;
    c = (unsigned char) lw[n - 1];
    if (c == (unsigned char) 'а' || c == (unsigned char) 'я')
        return G_F;
    if (c == (unsigned char) 'о' || c == (unsigned char) 'е'
        || c == (unsigned char) 'ё')
        return G_N;
    return G_M; /* consonants, й, and ь unless marked feminine */
}

/* genitive plural with no ending: стрел, палок, кошек */
static void
zero_gen(s, sl, out)
const char *s;
int sl;
char *out;
{
    char ls[BUFSZ];
    int c1, c2;

    Strcpy(out, "");
    if (sl >= BUFSZ - 4)
        sl = BUFSZ - 4;
    lowcopy(ls, s, sl);
    c1 = sl >= 1 ? (unsigned char) ls[sl - 1] : 0;
    c2 = sl >= 2 ? (unsigned char) ls[sl - 2] : 0;
    if (c1 == (unsigned char) 'к' && c2 && !is_vowel(c2)) {
        if (c2 == (unsigned char) 'ь' || c2 == (unsigned char) 'й') {
            (void) strncat(out, s, sl - 2);
            Strcat(out, "е");
        } else {
            (void) strncat(out, s, sl - 1);
            Strcat(out, is_sib(c2) ? "е" : "о");
        }
        Strcat(out, "к");
        return;
    }
    (void) strncat(out, s, sl);
}

static void noun_form1(const char *, int, const char *, int, unsigned, int,
                       int, int, char *);

/* decline noun w[0..n) (hint ann) to case cs, plural pl */
static void
noun_form(w, n, ann, annl, fl, g, cs, pl, out)
const char *w, *ann;
int n, annl;
unsigned fl;
int g, cs, pl;
char *out;
{
    const char *hy;
    int k;

    /* hyphenated: each part declines (пчела-убийца -> пчелы-убийцы);
       the hint is for the last part, the gender flags for the first */
    hy = (const char *) ru_memchr(w, '-', n);
    if (hy && hy > w && hy < w + n - 1) {
        char part[BUFSZ];
        const char *p = w, *q;

        out[0] = '\0';
        for (k = 0; p < w + n; k++) {
            int pl_len;

            q = (const char *) ru_memchr(p, '-', (w + n) - p);
            if (!q)
                q = w + n;
            pl_len = (int) (q - p);
            if (pl_len >= 3 && word_cyr(p, pl_len)) {
                char lw[BUFSZ];
                int pg;

                lowcopy(lw, p, pl_len);
                pg = k ? noun_gender(lw, pl_len, 0) : g;
                noun_form1(p, pl_len, q == w + n ? ann : 0,
                           q == w + n ? annl : 0,
                           k ? (fl & F_ANIM) : fl, pg, cs, pl, part);
            } else {
                Strcpy(part, "");
                (void) strncat(part, p, pl_len);
            }
            Strcat(out, part);
            if (q < w + n)
                Strcat(out, "-");
            p = q + 1;
        }
        return;
    }
    noun_form1(w, n, ann, annl, fl, g, cs, pl, out);
}

static void
noun_form1(w, n, ann, annl, fl, g, cs, pl, out)
const char *w, *ann;
int n, annl;
unsigned fl;
int g, cs, pl;
char *out;
{
    char lw[BUFSZ], stem[BUFSZ], pst[BUFSZ];
    const char *f[12];
    int flen[12], nf, para, c, sl, last;
    boolean anim = (fl & F_ANIM) != 0, up;
    boolean soft_pl = FALSE, have_pl = FALSE;
    const char *pnom = 0, *pgen = 0;
    int pnoml = 0, pgenl = 0, psl = 0;

    out[0] = '\0';
    if (n <= 0 || n >= BUFSZ - 8 || (fl & F_INDC) || !word_cyr(w, n)) {
        (void) strncat(out, w, n);
        return;
    }
    up = is_upper(w[0]);
    if (fl & F_ADJN) {
        adj_form(w, n, pl ? G_PL : adj_gender(w, n), cs, anim, out);
        return;
    }
    lowcopy(lw, w, n);
    nf = split_hint(ann, annl, f, flen);
    if (nf >= 12) {
        int idx = (pl ? 6 : 0) + cs;

        (void) strncat(out, f[idx], flen[idx]);
        if (up)
            out[0] = (char) ru_upper(out[0]);
        return;
    }
    if (fl & F_PLT)
        pl = 1;

    last = (unsigned char) lw[n - 1];
    if (fl & F_PLT)
        para = P_PLT;
    else if (last == (unsigned char) 'а')
        para = P_A;
    else if (last == (unsigned char) 'я')
        para = P_JA;
    else if (last == (unsigned char) 'о')
        para = P_O;
    else if (last == (unsigned char) 'е' || last == (unsigned char) 'ё')
        para = (n >= 2 && lw[n - 2] == (char) 'и') ? P_IE
               : (n >= 2 && lw[n - 2] == (char) 'ь') ? P_JE : P_E;
    else if (last == (unsigned char) 'ь')
        para = (g == G_F) ? P_FSOFT : P_MSOFT;
    else if (last == (unsigned char) 'й')
        para = P_J;
    else if (is_vowel(last)) { /* и у ы ю э: indeclinable */
        (void) strncat(out, w, n);
        return;
    } else
        para = P_MHARD;

    /* stem */
    sl = (para == P_MHARD) ? n : n - 1;
    Strcpy(stem, "");
    if (nf >= 1 && flen[0] > 0 && para != P_PLT) {
        (void) strncat(stem, f[0], flen[0]);
        sl = flen[0];
    } else {
        (void) strncat(stem, w, sl);
    }
    c = sl > 0 ? ru_lower(stem[sl - 1]) : 0;

    /* irregular plural from the hint */
    if (nf >= 3 && flen[1] > 0) {
        have_pl = TRUE;
        pnom = f[1], pnoml = flen[1];
        pgen = f[2], pgenl = flen[2];
        psl = pnoml - 1;
        Strcpy(pst, "");
        (void) strncat(pst, pnom, psl);
        soft_pl = (ru_lower(pnom[pnoml - 1]) == (unsigned char) 'я');
    }

    if (pl) {
        const char *pe;

        if (para == P_PLT) {
            /* plural-only: w is the nominative plural */
            Strcpy(pst, "");
            (void) strncat(pst, w, n - 1);
            psl = n - 1;
            switch (cs) {
            case C_NOM:
                (void) strncat(out, w, n);
                break;
            case C_GEN:
            gen_plt:
                if (nf >= 1 && flen[0] > 0)
                    (void) strncat(out, f[0], flen[0]);
                else {
                    Strcpy(out, pst);
                    Strcat(out, last == (unsigned char) 'ы' ? "ов" : "ей");
                }
                break;
            case C_ACC:
                if (anim)
                    goto gen_plt;
                (void) strncat(out, w, n);
                break;
            case C_DAT:
                Strcpy(out, pst), Strcat(out, "ам");
                break;
            case C_INS:
                Strcpy(out, pst), Strcat(out, "ами");
                break;
            default:
                Strcpy(out, pst), Strcat(out, "ах");
                break;
            }
            goto done;
        }
        if (cs == C_ACC)
            cs = anim ? C_GEN : C_NOM;
        if (have_pl) {
            switch (cs) {
            case C_NOM:
                (void) strncat(out, pnom, pnoml);
                break;
            case C_GEN:
                (void) strncat(out, pgen, pgenl);
                break;
            default:
                Strcpy(out, pst);
                Strcat(out, cs == C_DAT ? (soft_pl ? "ям" : "ам")
                            : cs == C_INS ? (soft_pl ? "ями" : "ами")
                              : (soft_pl ? "ях" : "ах"));
                break;
            }
            goto done;
        }
        /* regular plural */
        switch (para) {
        case P_MHARD:
            if (cs == C_NOM)
                pe = (is_vel(c) || is_sib(c)) ? "и" : "ы";
            else if (cs == C_GEN)
                pe = is_sib(c) ? "ей" : "ов";
            else
                pe = cs == C_DAT ? "ам" : cs == C_INS ? "ами" : "ах";
            break;
        case P_MSOFT:
        case P_FSOFT:
            if (cs == C_NOM)
                pe = "и";
            else if (cs == C_GEN)
                pe = "ей";
            else if (is_sib(c))
                pe = cs == C_DAT ? "ам" : cs == C_INS ? "ами" : "ах";
            else
                pe = cs == C_DAT ? "ям" : cs == C_INS ? "ями" : "ях";
            break;
        case P_J:
            if (cs == C_NOM)
                pe = "и";
            else if (cs == C_GEN)
                pe = "ев";
            else
                pe = cs == C_DAT ? "ям" : cs == C_INS ? "ями" : "ях";
            break;
        case P_A:
            if (cs == C_NOM)
                pe = (is_vel(c) || is_sib(c)) ? "и" : "ы";
            else if (cs == C_GEN) {
                zero_gen(stem, sl, out);
                goto done;
            } else
                pe = cs == C_DAT ? "ам" : cs == C_INS ? "ами" : "ах";
            break;
        case P_JA:
            if (cs == C_NOM)
                pe = "и";
            else if (cs == C_GEN) {
                if (c == (unsigned char) 'и' || is_vowel(c)) {
                    Strcpy(out, stem), Strcat(out, "й");
                } else if (c == (unsigned char) 'ь') {
                    (void) strncat(out, stem, sl - 1);
                    Strcat(out, "ей");
                } else {
                    Strcpy(out, stem), Strcat(out, "ь");
                }
                goto done;
            } else
                pe = cs == C_DAT ? "ям" : cs == C_INS ? "ями" : "ях";
            break;
        case P_O:
        case P_E:
            if (cs == C_NOM)
                pe = "а";
            else if (cs == C_GEN) {
                zero_gen(stem, sl, out);
                goto done;
            } else
                pe = cs == C_DAT ? "ам" : cs == C_INS ? "ами" : "ах";
            break;
        case P_IE:
            if (cs == C_NOM)
                pe = "я";
            else if (cs == C_GEN)
                pe = "й";
            else
                pe = cs == C_DAT ? "ям" : cs == C_INS ? "ями" : "ях";
            break;
        case P_JE:
            if (cs == C_NOM)
                pe = "я";
            else if (cs == C_GEN) {
                (void) strncat(out, stem, sl - 1);
                Strcat(out, "ий");
                goto done;
            } else
                pe = cs == C_DAT ? "ям" : cs == C_INS ? "ями" : "ях";
            break;
        default:
            pe = "";
            break;
        }
        Strcpy(out, stem);
        Strcat(out, pe);
        goto done;
    }

    /* singular */
    switch (para) {
    case P_MHARD:
    case P_MSOFT:
    case P_J:
        if (cs == C_ACC)
            cs = anim ? C_GEN : C_NOM;
        if (cs == C_NOM) {
            (void) strncat(out, w, n);
            goto done;
        }
        Strcpy(out, stem);
        if (para == P_MHARD)
            Strcat(out, cs == C_GEN ? "а" : cs == C_DAT ? "у"
                        : cs == C_INS ? "ом" : "е");
        else
            Strcat(out, cs == C_GEN ? "я" : cs == C_DAT ? "ю"
                        : cs == C_INS ? "ем"
                          : (para == P_J && c == (unsigned char) 'и') ? "и"
                            : "е");
        break;
    case P_A:
        if (cs == C_NOM) {
            (void) strncat(out, w, n);
            goto done;
        }
        Strcpy(out, stem);
        Strcat(out, cs == C_GEN ? ((is_vel(c) || is_sib(c)) ? "и" : "ы")
                    : cs == C_DAT ? "е" : cs == C_ACC ? "у"
                      : cs == C_INS ? ((is_sib(c) || c == (unsigned char) 'ц')
                                       ? "ей" : "ой")
                        : "е");
        break;
    case P_JA:
        if (cs == C_NOM) {
            (void) strncat(out, w, n);
            goto done;
        }
        Strcpy(out, stem);
        Strcat(out, cs == C_GEN ? "и"
                    : cs == C_ACC ? "ю" : cs == C_INS ? "ей"
                      : (c == (unsigned char) 'и') ? "и" : "е");
        break;
    case P_FSOFT:
        if (cs == C_NOM || cs == C_ACC) {
            (void) strncat(out, w, n);
            goto done;
        }
        Strcpy(out, stem);
        Strcat(out, cs == C_INS ? "ью" : "и");
        break;
    case P_O:
    case P_E:
    case P_IE:
    case P_JE:
        if (cs == C_NOM || cs == C_ACC) {
            (void) strncat(out, w, n);
            goto done;
        }
        Strcpy(out, stem);
        Strcat(out, cs == C_GEN ? (para == P_O || para == P_E ? "а" : "я")
                    : cs == C_DAT ? (para == P_O || para == P_E ? "у" : "ю")
                      : cs == C_INS ? (para == P_O ? "ом" : "ем")
                        : (para == P_IE ? "и" : "е"));
        break;
    default:
        (void) strncat(out, w, n);
        break;
    }
 done:
    if (up && out[0])
        out[0] = (char) ru_upper(out[0]);
}

/* ---------------------------------------------------------------- */
/* tokens */

enum { T_WORD, T_SPACE, T_PUNCT, T_CASE };

#define MAXTOK 400
#define RBUFSZ (BUFSZ * 4)

struct rtok {
    const char *s; /* word text */
    const char *ann;
    short n, annl;
    unsigned short fl;
    char type, cs;
    boolean used; /* case mark consumed */
    char *out;    /* declined form, or 0 */
};

static struct rtok toks[MAXTOK];
static int ntoks;
static char outpool[RBUFSZ * 2];
static int outpool_used;

static const char case_letters[] = "ирдвтп";

static int
case_of(c)
int c;
{
    const char *p = c ? index(case_letters, (char) c) : 0;

    return p ? (int) (p - case_letters) : -1;
}

static boolean
word_char(c)
int c;
{
    c &= 0xff;
    return (boolean) (ru_is_cyr(c) || (c >= 'a' && c <= 'z')
                      || (c >= 'A' && c <= 'Z') || digit((char) c)
                      || c == '-' || c == '+' || c == '\'');
}

static unsigned
mark_flag(c)
int c;
{
    switch (c) {
    case RU_HEAD: return F_HEAD;
    case RU_ANIM: return F_ANIM;
    case RU_PL: return F_PL;
    case RU_FEM: return F_FEM;
    case RU_NEU: return F_NEU;
    case RU_MASC: return F_MASC;
    case RU_PLT: return F_PLT;
    case RU_INDC: return F_INDC;
    case RU_ADJN: return F_ADJN;
    case RU_APPS: return F_APPS;
    }
    return 0;
}

static int
tokenize(in)
const char *in;
{
    int nt = 0;
    const char *p = in;

    while (*p && nt < MAXTOK - 1) {
        struct rtok *t = &toks[nt];
        int c = (unsigned char) *p;

        t->ann = 0, t->annl = 0, t->fl = 0, t->used = FALSE, t->out = 0;
        t->cs = 0;
        if (c == '@' && case_of((unsigned char) p[1]) >= 0) {
            t->type = T_CASE;
            t->cs = (char) case_of((unsigned char) p[1]);
            t->s = p, t->n = 2;
            p += 2;
            nt++;
            continue;
        }
        if (is_ru_mark(c)) {
            /* marks with no word before them: attach to the last word */
            if (nt > 0 && toks[nt - 1].type == T_WORD)
                toks[nt - 1].fl |= mark_flag(c);
            p++;
            continue;
        }
        if (c == '{') {
            const char *e = index(p, '}');

            if (nt > 0 && toks[nt - 1].type == T_WORD && e) {
                toks[nt - 1].ann = p + 1;
                toks[nt - 1].annl = (short) (e - p - 1);
            }
            p = e ? e + 1 : p + 1;
            continue;
        }
        if (c == ' ') {
            t->type = T_SPACE, t->s = p, t->n = 1;
            p++;
            nt++;
            continue;
        }
        if (word_char(c)) {
            const char *s = p;

            while (*p && word_char((unsigned char) *p))
                p++;
            t->type = T_WORD, t->s = s, t->n = (short) (p - s);
            nt++;
            continue;
        }
        t->type = T_PUNCT, t->s = p, t->n = 1;
        p++;
        nt++;
    }
    if (*p) { /* too long: the rest as one plain token */
        struct rtok *t = &toks[nt];

        t->type = T_PUNCT, t->s = p, t->n = (short) strlen(p);
        t->ann = 0, t->annl = 0, t->fl = 0, t->out = 0, t->used = FALSE;
        nt++;
    }
    return nt;
}

static char *
pool_alloc(len)
int len;
{
    char *r;

    if (outpool_used + len + 1 > (int) sizeof outpool)
        return 0;
    r = &outpool[outpool_used];
    outpool_used += len + 1;
    return r;
}

static void
set_out(t, s)
struct rtok *t;
const char *s;
{
    char *o = pool_alloc((int) strlen(s));

    if (o) {
        Strcpy(o, s);
        t->out = o;
    }
}

/* 1, 21 -> ONE; 2-4, 22-24 -> FEW; else MANY */
enum { N_ONE, N_FEW, N_MANY };

static int
numform(n)
long n;
{
    long a = n % 100, b = n % 10;

    if (b == 1 && a != 11)
        return N_ONE;
    if (b >= 2 && b <= 4 && (a < 12 || a > 14))
        return N_FEW;
    return N_MANY;
}

/* decline the phrase with head h; adjectives at adj[0..na); case cs */
static void
decline_phrase(h, adj, na, cs, count)
int h, na, cs;
int *adj;
long count;
{
    struct rtok *t = &toks[h];
    char lw[BUFSZ], buf[BUFSZ];
    int g, ncs, apl, acs, i, j;
    boolean npl, anim = (t->fl & F_ANIM) != 0, plt = (t->fl & F_PLT) != 0;
    boolean nomlike;
    int ag; /* adjective gender slot */

    if (!word_cyr(t->s, t->n) || t->n >= BUFSZ - 1)
        return;
    lowcopy(lw, t->s, t->n);
    g = (t->fl & F_ADJN) ? adj_gender(t->s, t->n) : noun_gender(lw, t->n, t->fl);
    if (g == G_PL)
        plt = TRUE;
    npl = plt || (t->fl & F_PL) != 0;
    ncs = cs;
    acs = cs;
    apl = npl;
    if (count >= 0) {
        int nf = numform(count);

        nomlike = (cs == C_NOM || (cs == C_ACC && !anim));
        if (nf == N_ONE) {
            npl = plt;
            apl = plt;
        } else if (nomlike) {
            if (nf == N_FEW && !plt) {
                npl = FALSE, ncs = C_GEN;
                apl = TRUE, acs = (g == G_F) ? C_NOM : C_GEN;
                if (t->fl & F_ADJN) /* 3 адские гончие, 3 дозорных */
                    npl = TRUE, ncs = acs;
            } else {
                npl = TRUE, ncs = C_GEN;
                apl = TRUE, acs = C_GEN;
            }
        } else if (cs == C_ACC) { /* animate */
            npl = TRUE, ncs = C_GEN;
            apl = TRUE, acs = C_GEN;
        } else {
            npl = TRUE, apl = TRUE;
        }
    }
    noun_form(t->s, t->n, t->ann, t->annl, t->fl, g, ncs, npl ? 1 : 0, buf);
    set_out(t, buf);
    ag = apl ? G_PL : (g == G_PL ? G_PL : g);
    for (i = 0; i < na; i++) {
        struct rtok *a = &toks[adj[i]];

        adj_form(a->s, a->n, ag, acs, anim, buf);
        set_out(a, buf);
    }
    /* appositions after the head: лорд Карнарвон */
    for (j = h + 1; j < ntoks && toks[j].type != T_PUNCT; j++) {
        struct rtok *ap = &toks[j];

        if (ap->type == T_SPACE)
            continue;
        if (ap->type != T_WORD || !(ap->fl & F_APPS))
            break;
        lowcopy(lw, ap->s, ap->n);
        noun_form(ap->s, ap->n, ap->ann, ap->annl, ap->fl,
                  (ap->fl & F_ADJN) ? adj_gender(ap->s, ap->n)
                                    : noun_gender(lw, ap->n, ap->fl),
                  ncs, npl ? 1 : 0, buf);
        set_out(ap, buf);
    }
}

#define MAXADJ 12

static void
resolve(nt)
int nt;
{
    int i, k, adj[MAXADJ], na;
    long count;

    /* phrases with a marked head */
    for (i = 0; i < nt; i++) {
        int cs = C_NOM;

        if (toks[i].type != T_WORD || !(toks[i].fl & F_HEAD)
            || (toks[i].fl & F_APPS))
            continue;
        na = 0;
        count = -1;
        for (k = i - 1; k >= 0; k--) {
            struct rtok *t = &toks[k];

            if (t->type == T_SPACE)
                continue;
            if (t->type == T_CASE) {
                if (!t->used) {
                    cs = t->cs;
                    t->used = TRUE;
                }
                break;
            }
            if (t->type != T_WORD || (t->fl & F_HEAD) || t->out)
                break;
            if (is_number(t->s, t->n)) {
                if (count < 0)
                    count = atol(t->s);
                continue;
            }
            if (is_ench(t->s, t->n))
                continue;
            if (is_many(t->s, t->n)) {
                if (count < 0)
                    count = 5;
                continue;
            }
            if (is_adjlike(t->s, t->n)) {
                if (na < MAXADJ)
                    adj[na++] = k;
                continue;
            }
            if (is_adverb(t->s, t->n))
                continue;
            break;
        }
        decline_phrase(i, adj, na, cs, count);
    }

    /* case marks with a plain phrase after them */
    for (i = 0; i < nt; i++) {
        int h = -1;

        if (toks[i].type != T_CASE || toks[i].used)
            continue;
        toks[i].used = TRUE;
        na = 0;
        count = -1;
        for (k = i + 1; k < nt; k++) {
            struct rtok *t = &toks[k];

            if (t->type == T_SPACE)
                continue;
            if (t->type != T_WORD || t->out)
                break;
            if (is_number(t->s, t->n)) {
                if (count < 0)
                    count = atol(t->s);
                continue;
            }
            if (is_ench(t->s, t->n))
                continue;
            if (is_many(t->s, t->n)) {
                if (count < 0)
                    count = 5;
                continue;
            }
            if ((t->fl & F_HEAD) || !is_adjlike(t->s, t->n)) {
                if (!is_adverb(t->s, t->n) || (t->fl & F_HEAD)) {
                    h = k;
                    break;
                }
                continue;
            }
            if (na < MAXADJ)
                adj[na++] = k;
        }
        if (h >= 0) {
            decline_phrase(h, adj, na, toks[i].cs, count);
        } else if (na > 0) {
            /* only adjectives: a predicate like "@т%s" with "голодный" */
            int j;
            char buf[BUFSZ];

            for (j = 0; j < na; j++) {
                struct rtok *a = &toks[adj[j]];

                adj_form(a->s, a->n, adj_gender(a->s, a->n), toks[i].cs,
                         FALSE, buf);
                set_out(a, buf);
            }
        }
    }
}

static boolean
needs_render(s)
const char *s;
{
    for (; *s; s++)
        if (is_ru_mark(*s) || *s == '{' || *s == '@')
            return TRUE;
    return FALSE;
}

void
ru_render_to(in, out, outsz)
const char *in;
char *out;
int outsz;
{
    int nt, i, len = 0;

    if (!in) {
        *out = '\0';
        return;
    }
    if (!needs_render(in)) {
        (void) strncpy(out, in, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    outpool_used = 0;
    nt = ntoks = tokenize(in);
    resolve(nt);
    for (i = 0; i < nt; i++) {
        struct rtok *t = &toks[i];
        const char *s;
        int n;

        if (t->type == T_CASE)
            continue;
        if (t->out)
            s = t->out, n = (int) strlen(t->out);
        else
            s = t->s, n = t->n;
        if (len + n >= outsz)
            n = outsz - 1 - len;
        if (n <= 0)
            break;
        memcpy(out + len, s, n);
        len += n;
    }
    out[len] = '\0';
}

const char *
ru_render(in)
const char *in;
{
    static char bufs[3][RBUFSZ];
    static int bi = 0;
    char *b;

    if (!in || !needs_render(in))
        return in;
    bi = (bi + 1) % 3;
    b = bufs[bi];
    ru_render_to(in, b, RBUFSZ);
    return b;
}

/* ---------------------------------------------------------------- */
/* helpers for name building */

static char rbufs[6][BUFSZ];
static int rbi = 0;

static char *
next_rbuf()
{
    rbi = (rbi + 1) % 6;
    return rbufs[rbi];
}

/* capitalize the first letter, skipping marks */
char *
ru_upstart(s)
char *s;
{
    char *p = s;

    if (!s)
        return s;
    while (*p && ((unsigned char) *p < ' ' || *p == '@')) {
        if (*p == '@' && case_of((unsigned char) p[1]) >= 0)
            p += 2;
        else if (*p == '@')
            break;
        else
            p++;
    }
    if (*p)
        *p = (char) ru_upper((unsigned char) *p);
    return s;
}

/* find the head of a name: pointer just past the head word and its
   hint, and whether it was marked */
static char *
find_head(s, marked)
char *s;
boolean *marked;
{
    char *p, *ws, *we;

    *marked = FALSE;
    if ((p = index(s, RU_HEAD)) != 0) {
        *marked = TRUE;
        return p;
    }
    /* first plain noun: not an adjective, number or adverb */
    for (p = s; *p;) {
        while (*p && !word_char((unsigned char) *p))
            p++;
        if (!*p)
            break;
        ws = p;
        while (*p && word_char((unsigned char) *p))
            p++;
        we = p;
        if (*p == '{') {
            char *e = index(p, '}');

            p = e ? e + 1 : p + 1;
        }
        if (word_cyr(ws, (int) (we - ws)) && !is_number(ws, (int) (we - ws))
            && !is_ench(ws, (int) (we - ws))
            && !is_adjlike(ws, (int) (we - ws))
            && !is_adverb(ws, (int) (we - ws)))
            return p;
        if (*p == '(' || *p == ',')
            break;
    }
    return 0;
}

/* insert string ins at position pos of buffer b */
static void
insert_at(b, pos, ins)
char *b, *pos;
const char *ins;
{
    int il = (int) strlen(ins);

    if ((int) strlen(b) + il >= BUFSZ - 1)
        return;
    memmove(pos + il, pos, strlen(pos) + 1);
    memcpy(pos, ins, il);
}

char *
ru_plural(str)
const char *str;
{
    char *b = next_rbuf(), *h;
    boolean marked;

    (void) strncpy(b, str, BUFSZ - 1);
    b[BUFSZ - 1] = '\0';
    h = find_head(b, &marked);
    if (!h)
        return b;
    if (marked) {
        char *q;

        /* already plural? */
        for (q = h; *q && is_ru_mark(*q); q++)
            if (*q == RU_PL || *q == RU_PLT)
                return b;
        insert_at(b, h + 1, RUPL);
    } else {
        insert_at(b, h, RUH RUPL);
    }
    return b;
}

char *
ru_singular(str)
const char *str;
{
    char *b = next_rbuf(), *p, *q;

    (void) strncpy(b, str, BUFSZ - 1);
    b[BUFSZ - 1] = '\0';
    for (p = q = b; *p; p++)
        if (*p != RU_PL)
            *q++ = *p;
    *q = '\0';
    return b;
}

boolean
ru_is_plural(str)
const char *str;
{
    return (boolean) (str && (index(str, RU_PL) || index(str, RU_PLT)));
}

/* remove marks and hints in place (for plain-text uses) */
char *
ru_strip(s)
char *s;
{
    char *p, *q;

    for (p = q = s; *p; p++) {
        if (is_ru_mark(*p))
            continue;
        if (*p == '{') {
            char *e = index(p, '}');

            if (e) {
                p = e;
                continue;
            }
        }
        if (*p == '@' && case_of((unsigned char) p[1]) >= 0) {
            p++;
            continue;
        }
        *q++ = *p;
    }
    *q = '\0';
    return s;
}

/* ---------------------------------------------------------------- */
/* dictionary: source syntax -> marked strings */

/*
 * Dictionary entries are written readably and turned into marked names
 * once, at startup:
 *   head      the word with a {hint} or a '^' in front; otherwise the
 *             first word that isn't an adjective
 *   ~ж ~с ~м  gender, ~мн plural-only, ~н indeclinable,
 *   ~п        declined like an adjective (after the hint, if any)
 *   +word     apposition (лорд +Карнарвон)
 *   =text     no head at all: used as is ("лечения" in "зелье лечения")
 */
static char *
ru_prep(src, anim)
const char *src;
boolean anim;
{
    char buf[BUFSZ], word[BUFSZ];
    const char *p;
    int headidx = -1, wi, nwords = 0;
    char *res;

    if (!src)
        return (char *) 0;
    if (*src == '=') {
        res = (char *) alloc((unsigned) strlen(src));
        Strcpy(res, src + 1);
        return res;
    }
    /* find the head word index */
    for (p = src, wi = 0; *p; wi++) {
        const char *ws;
        boolean hint = FALSE, caret = FALSE, appos = FALSE;
        int n;

        while (*p == ' ')
            p++;
        if (!*p)
            break;
        ws = p;
        if (*p == '^')
            caret = TRUE;
        if (*p == '+')
            appos = TRUE;
        while (*p && *p != ' ') {
            if (*p == '{')
                hint = TRUE;
            p++;
        }
        n = (int) (p - ws);
        if (headidx < 0 && !appos) {
            const char *we = ws;

            while (we < p && *we != '{' && *we != '~')
                we++;
            if (caret || hint
                || (word_cyr(ws, (int) (we - ws))
                    && !is_adjlike(ws, (int) (we - ws))
                    && !is_number(ws, (int) (we - ws))))
                headidx = wi;
        }
        nwords++;
        (void) n;
    }
    buf[0] = '\0';
    for (p = src, wi = 0; *p; wi++) {
        const char *ws, *we, *te;
        unsigned fl = 0;
        boolean appos = FALSE;

        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (wi)
            Strcat(buf, " ");
        ws = p;
        while (*p && *p != ' ')
            p++;
        we = p;
        if (*ws == '^')
            ws++;
        if (*ws == '+')
            ws++, appos = TRUE;
        /* flags */
        te = we;
        {
            const char *t = (const char *) ru_memchr(ws, '~', we - ws);

            while (t) {
                const char *fe = t + 1;

                while (fe < we && *fe != '~')
                    fe++;
                if (fe - t - 1 == 1 && t[1] == (char) 'ж')
                    fl |= F_FEM;
                else if (fe - t - 1 == 1 && t[1] == (char) 'с')
                    fl |= F_NEU;
                else if (fe - t - 1 == 1 && t[1] == (char) 'м')
                    fl |= F_MASC;
                else if (fe - t - 1 == 2 && t[1] == (char) 'м'
                         && t[2] == (char) 'н')
                    fl |= F_PLT;
                else if (fe - t - 1 == 1 && t[1] == (char) 'н')
                    fl |= F_INDC;
                else if (fe - t - 1 == 1 && t[1] == (char) 'п')
                    fl |= F_ADJN;
                if (te == we)
                    te = t;
                t = (fe < we) ? fe : 0;
            }
        }
        Strcpy(word, "");
        (void) strncat(word, ws, te - ws);
        Strcat(buf, word);
        if (wi == headidx || appos) {
            char m[12];
            int k = 0;

            if (appos)
                m[k++] = RU_APPS;
            else
                m[k++] = RU_HEAD;
            if (anim)
                m[k++] = RU_ANIM;
            if (fl & F_FEM)
                m[k++] = RU_FEM;
            if (fl & F_NEU)
                m[k++] = RU_NEU;
            if (fl & F_MASC)
                m[k++] = RU_MASC;
            if (fl & F_PLT)
                m[k++] = RU_PLT;
            if (fl & F_INDC)
                m[k++] = RU_INDC;
            if (fl & F_ADJN)
                m[k++] = RU_ADJN;
            m[k] = '\0';
            Strcat(buf, m);
        }
    }
    res = (char *) alloc((unsigned) strlen(buf) + 1);
    Strcpy(res, buf);
    return res;
}

/* exported for names built in the source */
char *
ru_phrase(src)
const char *src;
{
    char *b = next_rbuf(), *r = ru_prep(src, FALSE);

    Strcpy(b, r);
    free((genericptr_t) r);
    return b;
}

#ifndef RUS_TEST
#include "rusdict.h"

static const char *
lookup2(tab, en)
const struct ru_pair *tab;
const char *en;
{
    int i;

    if (!en)
        return 0;
    for (i = 0; tab[i].en; i++)
        if (!strcmp(tab[i].en, en))
            return tab[i].ru;
    return 0;
}

const char *
ru_lookup_descr(cls, en)
int cls;
const char *en;
{
    int i;

    if (!en)
        return 0;
    for (i = 0; ru_objdescrs[i].en; i++)
        if (ru_objdescrs[i].cls == cls && !strcmp(ru_objdescrs[i].en, en))
            return ru_objdescrs[i].ru;
    return 0;
}

/* Russian object and monster names: off until the messages that use
   them are translated (objnam.c, do_name.c and the rest); until then
   ru_init() only keeps the English names for the parsers */
#define RU_NAMES 0

void
ru_init()
{
    static boolean done = FALSE;
    int i;
    const char *r;

    if (done)
        return;
    done = TRUE;
    for (i = 0; i < NUM_OBJECTS; i++) {
        int cls = objects[i].oc_class;

        en_oc_name[i] = obj_descr[i].oc_name;
        en_oc_descr[i] = obj_descr[i].oc_descr;
        if (!RU_NAMES)
            continue;
        if ((r = lookup2(ru_objnames, obj_descr[i].oc_name)) != 0)
            obj_descr[i].oc_name = ru_prep(r, FALSE);
        if ((r = ru_lookup_descr(cls, obj_descr[i].oc_descr)) != 0)
            obj_descr[i].oc_descr = ru_prep(r, FALSE);
    }
    for (i = 0; i < NUMMONS; i++) {
        en_mname[i] = mons[i].mname;
        if (!RU_NAMES)
            continue;
        if ((r = lookup2(ru_monnames, mons[i].mname)) != 0)
            mons[i].mname = ru_prep(r, TRUE);
    }
}

#else
char *ru_prep_test(const char *s, boolean a) { return ru_prep(s, a); }
#endif /* RUS_TEST */

/*rus.c*/
