/*
 * Minimal JSON helpers - just enough for this server's flat request bodies.
 *
 * A full JSON library would be a dependency for what amounts to reading five
 * string and number fields out of {"model":"...","text":"...","speed":1.2}.
 * These helpers are deliberately strict: they find "key" at the top level and
 * decode the value, and they report failure rather than guessing.
 */
#ifndef JSON_MIN_H
#define JSON_MIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Locate the value text for `key` in a NUL-terminated JSON object body.
   Returns a pointer into `json`, or NULL when the key is absent. */
const char *json_find(const char *json, const char *key);

/* Copy the string value of `key` into `out` (UTF-8). Handles \" \\ \/ \n \t \r
   \uXXXX (including surrogate pairs -> UTF-8). Returns 1 on success. */
int json_get_string(const char *json, const char *key, char *out, int out_size);

/* Read a numeric value. Returns 1 on success. */
int json_get_double(const char *json, const char *key, double *out);

/* Read an integer value. Returns 1 on success. */
int json_get_int(const char *json, const char *key, int *out);

/* Escape a UTF-8 string as a JSON string body (no surrounding quotes). */
void json_escape(const char *in, char *out, int out_size);

#ifdef __cplusplus
}
#endif

#endif /* JSON_MIN_H */
