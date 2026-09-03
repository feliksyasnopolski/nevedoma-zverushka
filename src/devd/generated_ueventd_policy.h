#ifndef GENERATED_UEVENTD_POLICY_H
#define GENERATED_UEVENTD_POLICY_H
#include <stddef.h>
#include <sys/types.h>
struct dev_policy { const char *pattern,*source; unsigned mode; uid_t uid; gid_t gid; unsigned line; };
struct sys_policy { const char *path,*attr,*source; unsigned mode; uid_t uid; gid_t gid; unsigned line; };
struct subsystem_policy { const char *name,*dirname; };
extern const struct dev_policy tb_dev_policy[]; extern const size_t tb_dev_policy_count;
extern const struct sys_policy tb_sys_policy[]; extern const size_t tb_sys_policy_count;
extern const struct subsystem_policy tb_subsystem_policy[]; extern const size_t tb_subsystem_policy_count;
extern const char *tb_firmware_dirs[]; extern const size_t tb_firmware_dir_count; extern const int tb_socket_rcvbuf;
#endif
