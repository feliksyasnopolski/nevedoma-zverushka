#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <sys/system_properties.h>

struct property_value_match {
    const char* expected_value;
    int matches;
};

static void record_property_value(void* cookie, const char* name,
                                  const char* value, uint32_t serial) {
    struct property_value_match* match = cookie;

    (void)name;
    (void)serial;
    match->matches = strcmp(value, match->expected_value) == 0;
}

static int property_matches(const prop_info* property,
                            const char* expected_value) {
    struct property_value_match match = {
        .expected_value = expected_value,
        .matches = 0,
    };

    __system_property_read_callback(property, record_property_value, &match);
    return match.matches;
}

static void print_usage(const char* program_name) {
    fprintf(stderr, "usage: %s PROPERTY EXPECTED_VALUE\n", program_name);
}

int main(int argc, char** argv) {
    const char* property_name;
    const char* expected_value;

    if (argc != 3) {
        print_usage(argv[0]);
        return 2;
    }

    property_name = argv[1];
    expected_value = argv[2];

    if (property_name[0] == '\0') {
        fprintf(stderr, "%s: property name must not be empty\n", argv[0]);
        return 2;
    }

    for (;;) {
        const prop_info* property = __system_property_find(property_name);

        if (property != NULL) {
            uint32_t property_serial;
            uint32_t new_serial;

            if (property_matches(property, expected_value)) {
                return 0;
            }

            property_serial = __system_property_serial(property);
            if (property_serial == UINT32_MAX) {
                fprintf(stderr, "%s: failed to read serial for property '%s': %s\n",
                        argv[0], property_name, strerror(errno));
                return 1;
            }

            if (!__system_property_wait(property, property_serial, &new_serial,
                                        NULL)) {
                fprintf(stderr, "%s: failed waiting for property '%s': %s\n",
                        argv[0], property_name, strerror(errno));
                return 1;
            }
        } else {
            uint32_t area_serial;
            uint32_t new_serial;

            area_serial = __system_property_area_serial();
            if (area_serial == UINT32_MAX) {
                fprintf(stderr, "%s: failed to read property-area serial: %s\n",
                        argv[0], strerror(errno));
                return 1;
            }

            /* A null prop_info makes bionic wait for any property-area change. */
            if (!__system_property_wait(NULL, area_serial, &new_serial, NULL)) {
                fprintf(stderr, "%s: failed waiting for property-area change: %s\n",
                        argv[0], strerror(errno));
                return 1;
            }
        }
    }
}
