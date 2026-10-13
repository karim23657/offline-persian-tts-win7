/*
 * user.dict - a small pronunciation dictionary: word -> replacement text.
 *
 * Why substitution and not phonemes: the sherpa-onnx C API takes plain text
 * and phonemizes it internally with espeak-ng.  It exposes no phoneme input
 * at all, and its `lexicon` field is ignored whenever `data_dir` is set -
 * which is always the case for the piper models this package ships.  The
 * honest way to let a user fix a mispronounced word is therefore to replace
 * it before synthesis: a phonetic respelling ("کامپیوتر" -> "کمپیوتر"), an
 * abbreviation expanded, a foreign name written the way it is really said.
 *
 * File format, UTF-8, one entry per line:
 *
 *     word = replacement
 *
 * '#' at the start of a line is a comment.  Matching is whole-word; ASCII
 * letters also match case-insensitively, Persian (multi-byte UTF-8) is
 * compared byte-wise.  The same file is used by the GUI, the web server and
 * say.exe, so an entry added in one place applies everywhere.
 */
#ifndef DICT_H
#define DICT_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DICT_MAX_ENTRIES 512
#define DICT_MAX_WORD    96     /* bytes, incl. NUL */
#define DICT_MAX_REPL    192    /* bytes, incl. NUL */

typedef struct {
    char word[DICT_MAX_WORD];
    char repl[DICT_MAX_REPL];
} DictEntry;

/*
 * Replace the in-memory dictionary with the contents of `path`.
 * A missing file clears the dictionary and returns 0 (not an error - most
 * installs have no entries yet).  Returns -1 only when an existing file
 * cannot be read.
 */
int DictLoad(const wchar_t *path);

/* ANSI-path variant for the console tool (say.exe works in the console's
   code page; the GUI and server pass wide paths). */
int DictLoadA(const char *path);

/* Write the current entries back to `path`.  0 on success, -1 on error. */
int DictSave(const wchar_t *path);

int DictCount(void);

/* Entry by index, or NULL when out of range. */
const DictEntry *DictGet(int index);

/* Add or replace one entry.  Returns 1 on success, 0 when full/invalid. */
int DictSet(const char *word, const char *repl);

/* Remove an entry by word.  Returns 1 when something was removed. */
int DictRemove(const char *word);

void DictClear(void);

/*
 * Apply every entry to `text` in place (capacity `cap`, NUL-terminated).
 * Returns the number of substitutions performed.  A substitution that would
 * not fit in `cap` is skipped rather than truncated.
 */
int DictApply(char *text, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DICT_H */
