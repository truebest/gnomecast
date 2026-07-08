#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "config_paths.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_join_path(void) {
    char out[NATIVE_PERSISTED_CONFIG_PATH_MAX];

    assert(native_config_join_path(out, sizeof(out), "/a/b", "c.json"));
    assert(strcmp(out, "/a/b/c.json") == 0);

    /* A trailing slash on the directory must not double up. */
    assert(native_config_join_path(out, sizeof(out), "/a/b/", "c.json"));
    assert(strcmp(out, "/a/b/c.json") == 0);

    /* Empty inputs are rejected. */
    assert(!native_config_join_path(out, sizeof(out), "", "c.json"));
    assert(!native_config_join_path(out, sizeof(out), "/a", ""));

    /* Overflow is reported, not truncated. */
    char tiny[4];
    assert(!native_config_join_path(tiny, sizeof(tiny), "/a/b", "c.json"));
}

static void test_copy_path(void) {
    char out[8];
    assert(native_config_copy_path(out, sizeof(out), "abc"));
    assert(strcmp(out, "abc") == 0);

    /* Exactly filling the buffer (7 chars + NUL) is fine; one more overflows. */
    assert(native_config_copy_path(out, sizeof(out), "1234567"));
    assert(!native_config_copy_path(out, sizeof(out), "12345678"));
    assert(!native_config_copy_path(out, sizeof(out), ""));
}

static void test_parent_dir(void) {
    char dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];

    assert(native_config_parent_dir("/a/b/c.json", dir, sizeof(dir)));
    assert(strcmp(dir, "/a/b") == 0);

    /* A top-level file keeps the root slash rather than emptying the dir. */
    assert(native_config_parent_dir("/c.json", dir, sizeof(dir)));
    assert(strcmp(dir, "/") == 0);

    /* No slash at all -> current directory. */
    assert(native_config_parent_dir("c.json", dir, sizeof(dir)));
    assert(strcmp(dir, ".") == 0);
}

static void test_candidate_dedupe_and_priority(void) {
    NativeConfigPathCandidates c;
    memset(&c, 0, sizeof(c));

    assert(native_config_add_candidate_path(&c, "/env/settings.json", true));
    assert(native_config_add_candidate_path(&c, "/disk/settings.json", false));
    assert(c.count == 2);
    /* Priority order is insertion order; index 0 stays highest priority. */
    assert(strcmp(c.paths[0], "/env/settings.json") == 0 && c.from_env[0] == true);
    assert(strcmp(c.paths[1], "/disk/settings.json") == 0 && c.from_env[1] == false);

    /* A duplicate is a no-op (returns true) and preserves the first from_env tag. */
    assert(native_config_add_candidate_path(&c, "/env/settings.json", false));
    assert(c.count == 2);
    assert(c.from_env[0] == true);

    /* NULL/empty never grow the list. */
    assert(!native_config_add_candidate_path(&c, NULL, false));
    assert(!native_config_add_candidate_path(&c, "", false));
    assert(c.count == 2);
}

static void test_candidate_capacity(void) {
    NativeConfigPathCandidates c;
    memset(&c, 0, sizeof(c));

    char path[64];
    for (unsigned i = 0; i < NATIVE_PERSISTED_CONFIG_MAX_CANDIDATES; i++) {
        (void)snprintf(path, sizeof(path), "/d%u/settings.json", i);
        assert(native_config_add_candidate_path(&c, path, false));
    }
    assert(c.count == NATIVE_PERSISTED_CONFIG_MAX_CANDIDATES);

    /* One more distinct path is refused once the list is full. */
    assert(!native_config_add_candidate_path(&c, "/overflow/settings.json", false));
    assert(c.count == NATIVE_PERSISTED_CONFIG_MAX_CANDIDATES);

    /* add_candidate_dir composes the filename; add_candidate_app_dir formats the appid. */
    NativeConfigPathCandidates d;
    memset(&d, 0, sizeof(d));
    assert(native_config_add_candidate_dir(&d, "/var/data", false));
    assert(strcmp(d.paths[0], "/var/data/" NATIVE_PERSISTED_CONFIG_FILENAME) == 0);
    assert(native_config_add_candidate_app_dir(&d, "/apps/%s", "com.example"));
    assert(strcmp(d.paths[1], "/apps/com.example/" NATIVE_PERSISTED_CONFIG_FILENAME) == 0);
    assert(d.from_env[1] == false);
}

static void make_base_dir(char *base, size_t cap) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) {
        tmp = "/tmp";
    }
    (void)snprintf(base, cap, "%s/gnomecast-cfgtestXXXXXX", tmp);
    assert(mkdtemp(base) != NULL);
}

static void test_mkdir_p(char *base) {
    char nested[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(nested, sizeof(nested), "%s/a/b/c", base);

    assert(native_config_mkdir_p(nested));
    struct stat st;
    assert(stat(nested, &st) == 0 && S_ISDIR(st.st_mode));

    /* Idempotent: creating an existing tree succeeds. */
    assert(native_config_mkdir_p(nested));

    /* A trailing slash is tolerated. */
    char trailing[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(trailing, sizeof(trailing), "%s/a/b/c/", base);
    assert(native_config_mkdir_p(trailing));

    /* "." and "/" are treated as already existing. */
    assert(native_config_mkdir_p("."));
    assert(native_config_mkdir_p("/"));

    /* A regular file in the path yields failure, not a silent success. */
    char filepath[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(filepath, sizeof(filepath), "%s/afile", base);
    FILE *f = fopen(filepath, "wb");
    assert(f && fputc('x', f) != EOF && fclose(f) == 0);
    assert(!native_config_mkdir_p(filepath));

    char under_file[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(under_file, sizeof(under_file), "%s/afile/child", base);
    assert(!native_config_mkdir_p(under_file));
}

static void test_dir_is_secure(char *base) {
    char secure[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(secure, sizeof(secure), "%s/secure", base);
    assert(native_config_mkdir_p(secure)); /* created 0700, owned by us */
    assert(native_config_dir_is_secure(secure));

    /* Group- or other-writable directories are rejected (someone else could tamper). */
    assert(chmod(secure, 0770) == 0);
    assert(!native_config_dir_is_secure(secure));
    assert(chmod(secure, 0777) == 0);
    assert(!native_config_dir_is_secure(secure));
    assert(chmod(secure, 0700) == 0);
    assert(native_config_dir_is_secure(secure));

    /* A nonexistent directory is not secure. */
    char missing[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(missing, sizeof(missing), "%s/does-not-exist", base);
    assert(!native_config_dir_is_secure(missing));

    /* A symlink pointing at an otherwise-secure directory is rejected (lstat, not stat), so an
     * attacker cannot redirect the credential file by pre-creating a predictable dir name as a
     * symlink to a directory we own. */
    char link_path[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(link_path, sizeof(link_path), "%s/seclink", base);
    if (symlink(secure, link_path) == 0) {
        assert(!native_config_dir_is_secure(link_path));
    }

    /* When running as root (as CI containers do), exercise the foreign-owner rejection branch:
     * a directory owned by another uid must be rejected even though root could write to it. */
    if (geteuid() == 0) {
        char foreign[NATIVE_PERSISTED_CONFIG_PATH_MAX];
        (void)snprintf(foreign, sizeof(foreign), "%s/foreign", base);
        assert(native_config_mkdir_p(foreign));
        if (chown(foreign, 1000, 1000) == 0) {
            assert(!native_config_dir_is_secure(foreign));
        }
    }
}

static void test_find_save_candidate(char *base) {
    /* candidate[0] lives under a regular file (parent cannot be a directory), so it is
     * unusable; candidate[1] is a good, secure directory and must be chosen -- proving the
     * finder skips unusable higher-priority entries by writability, uid-independently. */
    char afile[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(afile, sizeof(afile), "%s/blocker", base);
    FILE *f = fopen(afile, "wb");
    assert(f && fclose(f) == 0);

    char good_dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(good_dir, sizeof(good_dir), "%s/good", base);
    assert(native_config_mkdir_p(good_dir));

    NativeConfigPathCandidates c;
    memset(&c, 0, sizeof(c));
    char blocked_path[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    (void)snprintf(blocked_path, sizeof(blocked_path), "%s/blocker/settings.json", base);
    assert(native_config_add_candidate_path(&c, blocked_path, false));
    assert(native_config_add_candidate_dir(&c, good_dir, false));

    size_t index = 999;
    assert(native_config_find_persisted_save_candidate(&c, &index));
    assert(index == 1);

    /* dir_writable round-trips a probe file and cleans it up. */
    assert(native_config_dir_writable(good_dir));
}

int main(void) {
    test_join_path();
    test_copy_path();
    test_parent_dir();
    test_candidate_dedupe_and_priority();
    test_candidate_capacity();

    char base[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    make_base_dir(base, sizeof(base));
    test_mkdir_p(base);
    test_dir_is_secure(base);
    test_find_save_candidate(base);

    printf("test_config_paths: all assertions passed\n");
    return 0;
}
