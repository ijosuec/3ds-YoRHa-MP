#pragma once

typedef struct {
    char **paths;
    int count;
    int cap;
} Library;

int library_scan(Library *lib, const char *root);
void library_free(Library *lib);
