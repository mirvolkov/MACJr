/* NetHack 3.6  rus.h -- Russian localization (MacJr port) */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * The game's text is Russian, stored in the source as UTF-8 and compiled
 * to CP866 (-fexec-charset), the charset of the MacJr console.
 *
 * Names (objects, monsters, body parts, ...) are built in the nominative
 * and carry invisible marks; message templates say which case a name
 * takes; ru_render(), called by the tty window port on every string it
 * shows, declines the names and drops the marks.
 *
 * Marks inside a name, right after its head noun (the word that
 * declines; adjectives in front of it agree with it):
 *
 *   RU_HEAD  this word is the head noun
 *   RU_ANIM  animate (accusative = genitive)
 *   RU_PL    plural
 *   RU_FEM, RU_NEU, RU_MASC   gender, when the ending doesn't tell
 *   RU_PLT   plural-only noun (перчатки, латы)
 *   RU_INDC  indeclinable (пони, желе)
 *   RU_ADJN  a noun declined like an adjective (гончая, дозорный)
 *   RU_APPS  apposition: takes the case of the phrase before it
 *            (лорд Карнарвон -> лорда Карнарвона)
 *
 * A head noun may carry a hint in braces, never shown:
 *   {stem}                 oblique stem   (свиток{свитк} -> свитка)
 *   {stem;plnom;plgen}     plus irregular plural (рог{;рога;рогов})
 *   {12 forms}             И Р Д В Т П singular, then plural
 * For a plural-only noun, {plgen} is its genitive (перчатки{перчаток}).
 *
 * In a template "@р" (and @и @д @в @т @п) before a name puts it in that
 * case: "Вы бьёте @в%s." with mon_nam() -> "Вы бьёте шакала."
 * A count in front of a name agrees with it: "3 стрела" -> "3 стрелы".
 */
#ifndef RUS_H
#define RUS_H

#define RU_HEAD '\002'
#define RU_ANIM '\003'
#define RU_PL   '\004'
#define RU_FEM  '\005'
#define RU_NEU  '\006'
#define RU_MASC '\007'
#define RU_PLT  '\016'
#define RU_INDC '\017'
#define RU_ADJN '\020'
#define RU_APPS '\021'

/* string forms, for building names in the source: "свиток{свитк}" RUH */
#define RUH  "\002"
#define RUA  "\003"
#define RUPL "\004"
#define RUF  "\005"
#define RUN  "\006"
#define RUM  "\007"
#define RUPT "\016"
#define RUI  "\017"

#define is_ru_mark(c) ((c) == RU_HEAD || (c) == RU_ANIM || (c) == RU_PL \
                       || (c) == RU_FEM || (c) == RU_NEU || (c) == RU_MASC \
                       || (c) == RU_PLT || (c) == RU_INDC || (c) == RU_ADJN \
                       || (c) == RU_APPS)

extern void NDECL(ru_init);
extern const char *FDECL(ru_render, (const char *));
extern void FDECL(ru_render_to, (const char *, char *, int));
extern boolean FDECL(ru_has_cyr, (const char *));
extern boolean FDECL(ru_is_cyr, (int));
extern char *FDECL(ru_upstart, (char *));
extern char *FDECL(ru_plural, (const char *));
extern char *FDECL(ru_singular, (const char *));
extern boolean FDECL(ru_is_plural, (const char *));
extern char *FDECL(ru_strip, (char *));
extern char *FDECL(ru_phrase, (const char *));
extern const char *FDECL(ru_lookup_descr, (int, const char *));
extern const char *FDECL(ru_selword, (const char *));
extern const char *FDECL(ru_rsword, (const char *, int));

/* the English names, kept for parsing what the player types (wishes,
   genocide, naming) and for the encyclopedia */
extern const char *en_oc_name[];
extern const char *en_oc_descr[];
extern const char *en_mname[];
#define EN_OBJ_NAME(obj) (en_oc_name[(obj).oc_name_idx])
#define EN_OBJ_DESCR(obj) (en_oc_descr[(obj).oc_descr_idx])
#define EN_MNAME(ptr) (en_mname[monsndx(ptr)])

#endif /* RUS_H */
