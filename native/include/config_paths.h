#ifndef GNOMECAST_CONFIG_PATHS_H
#define GNOMECAST_CONFIG_PATHS_H

#include <stdbool.h>
#include <stddef.h>

/* SDL-free helpers for locating and validating the persisted-settings file.
 *
 * These are split out of main.c so the host test suite can exercise the path
 * joining, candidate ordering, directory creation, and security checks that
 * decide where the RDP credentials file is written and read from — none of
 * which need SDL and all of which have been the source of ordering/security
 * bugs. */

#define NATIVE_PERSISTED_CONFIG_FILENAME "settings.json"
#define NATIVE_PERSISTED_CONFIG_PATH_MAX 1024u
#define NATIVE_PERSISTED_CONFIG_MAX_CANDIDATES 8u

/* Ordered, de-duplicated list of settings-file paths to try. Index 0 is the
 * highest priority. `from_env` marks paths supplied by the user through an
 * environment override: those are trusted for reading even when they live on a
 * read-only or foreign-owned mount, whereas discovered paths must pass the
 * ownership/permission check before their contents (which include a plaintext
 * password) are written or trusted. */
typedef struct NativeConfigPathCandidates {
    char paths[NATIVE_PERSISTED_CONFIG_MAX_CANDIDATES][NATIVE_PERSISTED_CONFIG_PATH_MAX];
    bool from_env[NATIVE_PERSISTED_CONFIG_MAX_CANDIDATES];
    size_t count;
} NativeConfigPathCandidates;

/* Join `dir` and `name` with exactly one '/'; false on overflow or empty input. */
bool native_config_join_path(char *path, size_t cap, const char *dir, const char *name);

/* Copy `src` into `dest`; false on overflow or empty input. */
bool native_config_copy_path(char *dest, size_t cap, const char *src);

/* Write the directory portion of `path` into `dir` ("." when there is no '/'). */
bool native_config_parent_dir(const char *path, char *dir, size_t cap);

/* mkdir -p for `dir`, creating each component 0700; treats an existing
 * directory as success. Does NOT validate ownership — callers that store
 * secrets must additionally check native_config_dir_is_secure(). */
bool native_config_mkdir_p(const char *dir);

/* True only when `dir` exists, is a directory, is owned by the effective UID,
 * and is not writable by group or other — i.e. no other user could have
 * planted or could replace a file inside it. */
bool native_config_dir_is_secure(const char *dir);

/* True when `dir` can be created/used AND is secure AND a probe file can be
 * written and removed there. Used to pick a directory safe for the credentials
 * file. */
bool native_config_dir_writable(const char *dir);

/* Append a fully-qualified settings-file path. `from_env` records whether it
 * came from a user-provided environment override. Returns true when the path is
 * present after the call (including when it was already there); false on
 * overflow or when the list is full. */
bool native_config_add_candidate_path(NativeConfigPathCandidates *candidates, const char *path, bool from_env);

/* Append <dir>/settings.json. See native_config_add_candidate_path for return. */
bool native_config_add_candidate_dir(NativeConfigPathCandidates *candidates, const char *dir, bool from_env);

/* Append <fmt % appid>/settings.json as a discovered (non-env) candidate. */
bool native_config_add_candidate_app_dir(NativeConfigPathCandidates *candidates, const char *fmt, const char *appid);

/* True when the parent directory of `path` is writable + secure. */
bool native_config_persisted_path_writable(const char *path);

/* Find the highest-priority candidate whose directory is writable + secure and
 * store its index. False when none qualifies. */
bool native_config_find_persisted_save_candidate(const NativeConfigPathCandidates *candidates, size_t *index);

#endif
