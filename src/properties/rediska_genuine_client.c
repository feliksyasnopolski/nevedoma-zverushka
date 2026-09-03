#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/system_properties.h>

typedef int (*init_fn)(void);
typedef unsigned (*serial_fn)(const prop_info*);
typedef int (*read_fn)(const prop_info*, char*, char*);
typedef int (*wait_fn)(const prop_info*, unsigned, unsigned*, const struct timespec*);

static void* symbol(const char* name) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (!p) { fprintf(stderr, "client: missing %s\n", name); return NULL; }
    return p;
}

int main(void) {
    init_fn init = (init_fn)symbol("__system_properties_init");
    serial_fn serial = (serial_fn)symbol("__system_property_serial");
    read_fn read = (read_fn)symbol("__system_property_read");
    wait_fn wait = (wait_fn)symbol("__system_property_wait");
    if (!init || !serial || !read || !wait || init() != 0) { puts("CLIENT_INIT_FAIL"); return 1; }
    const prop_info* seeded = __system_property_find("island.seed");
    char name[PROP_NAME_MAX + 1] = {0}, value[PROP_VALUE_MAX] = {0};
    if (!seeded || read(seeded, name, value) < 0) { puts("SEEDED_FAIL"); return 1; }
    printf("SEEDED name=%s value=%s serial=%u\n", name, value, serial(seeded));
    const prop_info* test = __system_property_find("island.test");
    if (!test) { puts("TEST_NOT_PRESENT_BEFORE_ADD"); }
    fflush(stdout);
    puts("CLIENT_READY"); fflush(stdout);
    char command[32];
    if (!fgets(command, sizeof(command), stdin)) return 1;
    test = __system_property_find("island.test");
    if (!test || read(test, name, value) < 0) { puts("ADD_READ_FAIL"); return 1; }
    printf("AFTER_ADD value=%s serial=%u\n", value, serial(test));
    uint32_t old = serial(test), now = 0;
    struct timespec timeout = {.tv_sec = 5, .tv_nsec = 0};
    puts("WAITING"); fflush(stdout);
    int woke = wait(test, old, &now, &timeout);
    memset(value, 0, sizeof(value));
    read(test, name, value);
    printf("WAIT_RESULT woke=%d old=%u new=%u value=%s\n", woke, old, now, value);
    return (woke && now != old && !strcmp(value, "331")) ? 0 : 1;
}
