#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "library.h"

#define MAX_DEPTH 4

static int has_mp3_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".mp3") == 0;
}

static void add_path(Library *lib, const char *path)
{
    if (lib->count == lib->cap) {
        int cap = lib->cap ? lib->cap * 2 : 64;
        char **p = realloc(lib->paths, cap * sizeof(char *));
        if (!p)
            return;
        lib->paths = p;
        lib->cap = cap;
    }
    char *copy = strdup(path);
    if (copy)
        lib->paths[lib->count++] = copy;
}

static void scan_dir(Library *lib, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

        int is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        }

        if (is_dir) {
            if (depth < MAX_DEPTH)
                scan_dir(lib, path, depth + 1);
        } else if (has_mp3_ext(e->d_name)) {
            add_path(lib, path);
        }
    }
    closedir(d);
}

static int compare_paths(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

int library_scan(Library *lib, const char *root)
{
    scan_dir(lib, root, 0);
    qsort(lib->paths, lib->count, sizeof(char *), compare_paths);
    return lib->count;
}

void library_free(Library *lib)
{
    for (int i = 0; i < lib->count; i++)
        free(lib->paths[i]);
    free(lib->paths);
    lib->paths = NULL;
    lib->count = lib->cap = 0;
}
