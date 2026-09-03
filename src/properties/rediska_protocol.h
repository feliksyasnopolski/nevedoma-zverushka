#ifndef TB330FU_REDISKA_PROTOCOL_H
#define TB330FU_REDISKA_PROTOCOL_H

#include <stdint.h>

#define REDISKA_MAGIC 0x5244534bu /* RDSK */
#define REDISKA_VERSION 1u
#define REDISKA_SOCKET_DEFAULT "/run/tb330fu/rediska.sock"
#define REDISKA_NAME_MAX 128u
#define REDISKA_VALUE_MAX 92u

enum rediska_opcode { REDISKA_HELLO = 1, REDISKA_GET, REDISKA_SET, REDISKA_FIND,
                    REDISKA_READ, REDISKA_WAIT, REDISKA_WAIT_ANY };
enum rediska_status { REDISKA_OK = 0, REDISKA_NOT_FOUND = 1, REDISKA_BAD_REQUEST = 2,
                    REDISKA_INTERNAL = 3, REDISKA_TIMEOUT = 4 };

struct rediska_header {
    uint32_t magic, version, opcode, request_id, payload_len;
};

struct rediska_property {
    char name[REDISKA_NAME_MAX];
    char value[REDISKA_VALUE_MAX];
    uint32_t serial;
};

struct rediska_request {
    char name[REDISKA_NAME_MAX];
    char value[REDISKA_VALUE_MAX];
    uint32_t serial;
    uint64_t timeout_ns;
};

#define REDISKA_MAX_PAYLOAD ((uint32_t)sizeof(struct rediska_request))

#endif
