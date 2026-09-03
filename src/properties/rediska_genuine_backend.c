#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

typedef int (*area_init_fn)(void);
typedef int (*add_fn)(const char*, unsigned, const char*, unsigned);
typedef int (*update_fn)(prop_info*, const char*, unsigned);
typedef const prop_info* (*find_fn)(const char*);
typedef unsigned (*serial_fn)(const prop_info*);

static void* required(const char* name) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (!p) { fprintf(stderr, "rediska-area: missing %s: %s\n", name, dlerror()); exit(1); }
    return p;
}

static int seed_file(const char* path, add_fn add) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[512]; unsigned accepted = 0, rejected = 0, duplicate = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#') continue;
        char* eq = strchr(p, '=');
        if (!eq) { ++rejected; continue; }
        *eq++ = 0;
        char* end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        end = eq + strlen(eq);
        while (end > eq && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        size_t n = strlen(p), v = strlen(eq);
        if (!n || n >= 128 || v >= PROP_VALUE_MAX || add(p, (unsigned)n, eq, (unsigned)v) != 0) {
            ++rejected;
        } else {
            ++accepted;
        }
    }
    fclose(f);
    fprintf(stderr, "rediska-area: seed=%s accepted=%u rejected=%u duplicate_or_add_failure=%u\n",
            path, accepted, rejected, duplicate);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s SEED_FILE\n", argv[0]); return 2; }
    area_init_fn area_init = (area_init_fn)required("__system_property_area_init");
    add_fn add = (add_fn)required("__system_property_add");
    update_fn update = (update_fn)required("__system_property_update");
    find_fn find = (find_fn)required("__system_property_find");
    serial_fn serial = (serial_fn)required("__system_property_serial");
    if (area_init() != 0) { fprintf(stderr, "rediska-area: creator failed errno=%d\n", errno); return 1; }
    puts("AREA_READY"); fflush(stdout);
    seed_file(argv[1], add);
    puts("SEED_READY"); fflush(stdout);
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        char op[16], name[128], value[PROP_VALUE_MAX];
        if (sscanf(line, "%15s %127s %91[^\n]", op, name, value) < 2) continue;
        if (!strcmp(op, "ADD")) {
            int rc = add(name, (unsigned)strlen(name), value, (unsigned)strlen(value));
            fprintf(stderr, "rediska-area: add name=%s value=%s rc=%d\n", name, value, rc);
        } else if (!strcmp(op, "UPDATE")) {
            prop_info* pi = (prop_info*)find(name);
            unsigned before = pi ? serial(pi) : 0;
            int rc = pi ? update(pi, value, (unsigned)strlen(value)) : -1;
            unsigned after = pi ? serial(pi) : 0;
            fprintf(stderr, "rediska-area: update name=%s old_serial=%u new_serial=%u rc=%d\n",
                    name, before, after, rc);
        } else if (!strcmp(op, "STOP")) {
            break;
        }
        fflush(stderr);
    }
    return 0;
}
