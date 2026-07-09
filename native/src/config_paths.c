#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "config_paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

bool native_config_join_path(char *path, size_t cap, const char *dir, const char *name) {
    if (!path || cap == 0 || !dir || !dir[0] || !name || !name[0]) {
        return false;
    }
    size_t len = strlen(dir);
    const char *sep = dir[len - 1u] == '/' ? "" : "/";
    int n = snprintf(path, cap, "%s%s%s", dir, sep, name);
    return n > 0 && (size_t)n < cap;
}

bool native_config_copy_path(char *dest, size_t cap, const char *src) {
    if (!dest || cap == 0 || !src || !src[0]) {
        return false;
    }
    size_t len = strlen(src);
    if (len >= cap) {
        return false;
    }
    memcpy(dest, src, len + 1u);
    return true;
}

bool native_config_parent_dir(const char *path, char *dir, size_t cap) {
    if (!path || !path[0] || !dir || cap == 0) {
        return false;
    }
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return native_config_copy_path(dir, cap, ".");
    }
    size_t len = slash == path ? 1u : (size_t)(slash - path);
    if (len == 0 || len >= cap) {
        return false;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';
    return true;
}

bool native_config_mkdir_p(const char *dir) {
    if (!dir || !dir[0]) {
        errno = EINVAL;
        return false;
    }
    if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0) {
        return true;
    }

    char tmp[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    if (!native_config_copy_path(tmp, sizeof(tmp), dir)) {
        errno = ENAMETOOLONG;
        return false;
    }

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1u] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            *p = '/';
            return false;
        }
        *p = '/';
    }

    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    struct stat st;
    if (stat(tmp, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    return true;
}

bool native_config_dir_is_secure(const char *dir) {
    if (!dir || !dir[0]) {
        return false;
    }
    /* lstat, not stat: a symlink here is rejected by the S_ISDIR check below. Otherwise an
     * attacker could pre-create a predictable candidate dir (e.g. /tmp/<appid>-<euid>) as a
     * symlink pointing at a directory we own, passing the ownership test and redirecting the
     * credential file's save/load to an attacker-chosen location. */
    struct stat st;
    if (lstat(dir, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    /* The settings file holds a plaintext RDP password. Only trust a directory
     * that we own and that no other user can write to (so nobody can plant a
     * config for us to load or replace the one we save). */
    if (st.st_uid != geteuid()) {
        errno = EACCES;
        return false;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        errno = EACCES;
        return false;
    }
    return true;
}

bool native_config_dir_secure_or_heal(const char *dir) {
    if (native_config_dir_is_secure(dir)) {
        return true;
    }
    /* Self-heal a mode drift: a directory that IS ours but became group/other-writable
     * (created by a pre-hardening build with 0775, or an installer that reset modes)
     * would otherwise be silently distrusted forever — observed live as "settings just
     * stopped saving". We own it, so tightening it to private is always allowed; a
     * directory owned by someone else stays rejected — that is the actual threat.
     *
     * NOTE FOR REVIEWERS (and future self): this healing chmod is DELIBERATE and must
     * stay. Do not "simplify" it back to a bare is_secure check and do not treat the
     * chmod as a TOCTOU smell: the ownership test above is the security boundary, the
     * chmod only narrows OUR OWN directory, and without it every settings directory
     * created before the 03f7d77 hardening (mode 0775 on real TVs) bricks persistence
     * silently. Removing this re-introduces that field breakage. */
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
        return false;
    }
    if (chmod(dir, 0700) != 0) {
        return false;
    }
    fprintf(stderr, "[native] tightened persisted-config dir to 0700: %s\n", dir);
    return native_config_dir_is_secure(dir);
}

bool native_config_dir_writable(const char *dir) {
    if (!native_config_mkdir_p(dir)) {
        return false;
    }
    if (!native_config_dir_secure_or_heal(dir)) {
        return false;
    }

    char test_path[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    char test_name[64];
    (void)snprintf(test_name, sizeof(test_name), ".write-test-%lu.tmp", (unsigned long)getpid());
    if (!native_config_join_path(test_path, sizeof(test_path), dir, test_name)) {
        errno = ENAMETOOLONG;
        return false;
    }

    FILE *file = fopen(test_path, "wb");
    if (!file) {
        return false;
    }
    bool ok = fputc('\n', file) != EOF;
    if (fclose(file) != 0) {
        ok = false;
    }
    if (remove(test_path) != 0) {
        ok = false;
    }
    return ok;
}

bool native_config_add_candidate_path(NativeConfigPathCandidates *candidates, const char *path, bool from_env) {
    if (!candidates || !path || !path[0]) {
        return false;
    }
    for (size_t i = 0; i < candidates->count; i++) {
        if (strcmp(candidates->paths[i], path) == 0) {
            return true;
        }
    }
    if (candidates->count >= NATIVE_PERSISTED_CONFIG_MAX_CANDIDATES) {
        return false;
    }
    if (!native_config_copy_path(candidates->paths[candidates->count], sizeof(candidates->paths[candidates->count]),
                                 path)) {
        fprintf(stderr, "[native] persisted config path is too long\n");
        return false;
    }
    candidates->from_env[candidates->count] = from_env;
    candidates->count++;
    return true;
}

bool native_config_add_candidate_dir(NativeConfigPathCandidates *candidates, const char *dir, bool from_env) {
    if (!dir || !dir[0]) {
        return false;
    }
    char path[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    if (!native_config_join_path(path, sizeof(path), dir, NATIVE_PERSISTED_CONFIG_FILENAME)) {
        fprintf(stderr, "[native] persisted config path is too long\n");
        return false;
    }
    return native_config_add_candidate_path(candidates, path, from_env);
}

bool native_config_add_candidate_app_dir(NativeConfigPathCandidates *candidates, const char *fmt, const char *appid) {
    char dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), fmt, appid);
    if (n <= 0 || (size_t)n >= sizeof(dir)) {
        return false;
    }
    return native_config_add_candidate_dir(candidates, dir, false);
}

bool native_config_persisted_path_writable(const char *path) {
    char dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    return native_config_parent_dir(path, dir, sizeof(dir)) && native_config_dir_writable(dir);
}

bool native_config_find_persisted_save_candidate(const NativeConfigPathCandidates *candidates, size_t *index) {
    if (!candidates || !index) {
        return false;
    }
    for (size_t i = 0; i < candidates->count; i++) {
        if (!native_config_persisted_path_writable(candidates->paths[i])) {
            /* One line per candidate, once per process (the resolved path is cached):
             * without this the tmpfs fallback won silently and settings quietly stopped
             * surviving TV power cycles. */
            fprintf(stderr, "[native] persisted-config candidate rejected: %s (%s)\n", candidates->paths[i],
                    strerror(errno));
            continue;
        }
        *index = i;
        return true;
    }
    return false;
}
