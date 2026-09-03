#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#ifdef __linux__
#include <linux/netlink.h>
#else
/* Keep host-side source inspection/builds possible on Darwin. */
struct sockaddr_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int nl_pid;
    unsigned int nl_groups;
};
#define NETLINK_KOBJECT_UEVENT 15
#endif
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#else
#ifndef makedev
#define makedev(major, minor) ((dev_t)((((dev_t)(major)) << 8) | (dev_t)(minor)))
#endif
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#endif
#include <sys/types.h>
#include <unistd.h>
#include "generated_ueventd_policy.h"

#define DEV_ROOT "/dev"
#define SYS_ROOT "/sys"
#define UDEV_PAYLOAD_MAX (64U * 1024U)
#define FIELD_MAX PATH_MAX

struct managed_node {
    char *name;
    bool block;
    bool owned;
    unsigned int major;
    unsigned int minor;
    char *alias;
    char *alias_target;
    bool alias_owned;
    struct managed_node *next;
};

static volatile sig_atomic_t stopping;
static struct managed_node *managed;
static int dev_fd = -1;
static void log_msg(const char *fmt, ...);

static void notify_ready(void)
{
    const char *socket_path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd;
    if (!socket_path || !*socket_path) return;
    if (socket_path[0] == '@') {
        address.sun_path[0] = '\0';
        if (snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1, "%s", socket_path + 1) >= (int)sizeof(address.sun_path) - 1) return;
    } else if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path) >= (int)sizeof(address.sun_path)) return;
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    const char *message = "READY=1\nSTATUS=kernel device coldboot complete\n";
    (void)sendto(fd, message, strlen(message), MSG_NOSIGNAL,
                 (struct sockaddr *)&address, sizeof(address));
    close(fd);
}

static void apply_dev_policy(const char *name)
{
    char path[FIELD_MAX];
    const struct dev_policy *match = NULL;
    if (snprintf(path, sizeof(path), "/dev/%s", name) >= (int)sizeof(path)) return;
    /* Android's concrete rules are ordered; the last matching rule wins. */
    for (size_t i = 0; i < tb_dev_policy_count; ++i)
        if (fnmatch(tb_dev_policy[i].pattern, path, 0) == 0) match = &tb_dev_policy[i];
    if (!match) {
        int ignored_chmod = fchmodat(dev_fd, name, 0600, 0);
        int ignored_chown = fchownat(dev_fd, name, 0, 0, 0);
        (void)ignored_chmod; (void)ignored_chown;
        return;
    }
    if (fchmodat(dev_fd, name, (mode_t)match->mode, 0) < 0 || fchownat(dev_fd, name, match->uid, match->gid, 0) < 0)
        log_msg("/dev/%s metadata %04o %u:%u failed (%s:%u): %s", name, match->mode, (unsigned)match->uid, (unsigned)match->gid, match->source, match->line, strerror(errno));
}

static void apply_sys_policy(const char *devpath)
{
    char path[FIELD_MAX], target[FIELD_MAX];
    if (snprintf(path, sizeof(path), "%s%s", SYS_ROOT, devpath) >= (int)sizeof(path)) return;
    for (size_t i = 0; i < tb_sys_policy_count; ++i) {
        const struct sys_policy *p = &tb_sys_policy[i];
        if (fnmatch(p->path, path, 0) != 0) continue;
        if (snprintf(target, sizeof(target), "%s/%s", path, p->attr) >= (int)sizeof(target)) continue;
        if (chmod(target, (mode_t)p->mode) < 0 || chown(target, p->uid, p->gid) < 0)
            log_msg("sysfs %s metadata %04o %u:%u failed (%s:%u): %s", target, p->mode, (unsigned)p->uid, (unsigned)p->gid, p->source, p->line, strerror(errno));
    }
}

static const char *shape_name(const char *subsystem, const char *devpath, const char *devname)
{
    static char shaped[FIELD_MAX];
    for (size_t i = 0; i < tb_subsystem_policy_count; ++i) {
        if (!strcmp(tb_subsystem_policy[i].name, subsystem)) {
            const char *base = strrchr(devpath, '/');
            base = base ? base + 1 : devname;
            if (snprintf(shaped, sizeof(shaped), "%s/%s", tb_subsystem_policy[i].dirname + 5, base) >= (int)sizeof(shaped)) return NULL;
            return shaped;
        }
    }
    return devname;
}

static bool safe_firmware_name(const char *name)
{
    const char *p = name, *start = name;
    if (!name || !*name || *name == '/') return false;
    for (;; ++p) if (*p == '/' || *p == '\0') {
        size_t n = (size_t)(p - start);
        if (!n || (n == 1 && start[0] == '.') || (n == 2 && start[0] == '.' && start[1] == '.')) return false;
        if (*p == '\0') return true;
        start = p + 1;
    } else if ((unsigned char)*p < 0x20) return false;
}

static void firmware_event(const char *devpath, const char *firmware)
{
    char base[FIELD_MAX], loading[FIELD_MAX], data[FIELD_MAX], selected[FIELD_MAX];
    int lfd = -1, dfd = -1, src = -1; ssize_t n; bool ok = false;
    if (!devpath[0] || !safe_firmware_name(firmware)) { log_msg("rejecting malformed firmware request"); return; }
    if (snprintf(base, sizeof(base), "%s%s", SYS_ROOT, devpath) >= (int)sizeof(base) ||
        snprintf(loading, sizeof(loading), "%s/loading", base) >= (int)sizeof(loading) ||
        snprintf(data, sizeof(data), "%s/data", base) >= (int)sizeof(data)) return;
    lfd = open(loading, O_WRONLY | O_CLOEXEC); dfd = open(data, O_WRONLY | O_CLOEXEC);
    if (lfd < 0 || dfd < 0) goto done;
    if (write(lfd, "1", 1) != 1) goto done;
    for (size_t i = 0; i < tb_firmware_dir_count; ++i) {
        if (snprintf(selected, sizeof(selected), "%s%s", tb_firmware_dirs[i], firmware) >= (int)sizeof(selected)) continue;
        src = open(selected, O_RDONLY | O_CLOEXEC); if (src < 0) continue;
        char buf[8192];
        while ((n = read(src, buf, sizeof(buf))) > 0) { ssize_t off = 0; while (off < n) { ssize_t w = write(dfd, buf + off, (size_t)(n - off)); if (w < 0 && errno == EINTR) continue; if (w <= 0) goto done; off += w; } }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        ok = true; break;
    }
done:
    if (lfd >= 0) {
        const char *result = ok ? "0" : "-1";
        ssize_t ignored = write(lfd, result, strlen(result));
        (void)ignored;
    }
    if (src >= 0) close(src);
    if (dfd >= 0) close(dfd);
    if (lfd >= 0) close(lfd);
    if (ok) log_msg("firmware %s loaded from %s for %s", firmware, selected, devpath);
    else log_msg("firmware %s failed for %s (searched %zu configured paths)", firmware, devpath, tb_firmware_dir_count);
}

static void on_signal(int sig) { (void)sig; stopping = 1; }

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "tb330fu-devd: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static bool parse_uint(const char *s, unsigned int *out)
{
    char *end;
    unsigned long value;

    if (!s || !*s) return false;
    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno || *end || value > UINT_MAX) return false;
    *out = (unsigned int)value;
    return true;
}

static bool parse_devno(const char *text, unsigned int *major, unsigned int *minor)
{
    char extra;
    return sscanf(text, "%u:%u%c", major, minor, &extra) == 2;
}

static bool valid_name(const char *name)
{
    const char *p, *start;
    size_t length;

    if (!name || !*name || name[0] == '/') return false;
    length = strnlen(name, FIELD_MAX);
    if (length == 0 || length >= FIELD_MAX) return false;
    start = name;
    for (p = name;; p++) {
        if (*p == '\0' || *p == '/') {
            size_t part_len = (size_t)(p - start);
            if (part_len == 0 || (part_len == 1 && start[0] == '.') ||
                (part_len == 2 && start[0] == '.' && start[1] == '.'))
                return false;
            if (*p == '\0') break;
            start = p + 1;
        } else if ((unsigned char)*p < 0x20) {
            return false;
        }
    }
    return true;
}

static bool valid_partition_name(const char *name)
{
    const char *accept = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.";
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!strchr(accept, *p)) return false;
    return true;
}

static struct managed_node *find_managed(const char *name)
{
    for (struct managed_node *n = managed; n; n = n->next)
        if (!strcmp(n->name, name)) return n;
    return NULL;
}

static void forget_managed(const char *name)
{
    struct managed_node **pp = &managed;
    while (*pp) {
        if (!strcmp((*pp)->name, name)) {
            struct managed_node *old = *pp;
            *pp = old->next;
            free(old->name);
            free(old->alias);
            free(old->alias_target);
            free(old);
            return;
        }
        pp = &(*pp)->next;
    }
}

static bool remember_managed(const char *name, bool block, unsigned int major,
                             unsigned int minor, bool owned)
{
    struct managed_node *n = find_managed(name);
    if (!n) {
        n = calloc(1, sizeof(*n));
        if (!n || !(n->name = strdup(name))) {
            free(n);
            log_msg("out of memory adopting /dev/%s", name);
            return false;
        }
        n->next = managed;
        managed = n;
    }
    n->block = block;
    if (owned) n->owned = true;
    n->major = major;
    n->minor = minor;
    return true;
}

static void remember_alias(struct managed_node *n, const char *alias,
                           const char *target, bool owned)
{
    if (!n || !alias || !target) return;
    if (!n->alias) n->alias = strdup(alias);
    if (!n->alias_target) n->alias_target = strdup(target);
    if (n->alias && n->alias_target && owned) n->alias_owned = true;
}

/*
 * A coldboot entry is kernel state that existed before devd started; unlike a
 * netlink add, it has no add event left for udev to process.  Trigger the
 * event on the exact sysfs device represented by the already-validated
 * major:minor index entry.  This is deliberately best-effort: a disappearing
 * device or a read-only/missing uevent attribute must not stop unrelated
 * devices from being materialized.
 */
static void trigger_coldboot_add(const char *index, const char *devno,
                                 const char *devpath, const char *name)
{
    char uevent[PATH_MAX];
    int fd;
    ssize_t written;

    if (snprintf(uevent, sizeof(uevent), "%s/%s/uevent", index, devno) >=
        (int)sizeof(uevent)) {
        log_msg("coldboot add /dev/%s: sysfs uevent path is too long", name);
        return;
    }
    fd = open(uevent, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        log_msg("coldboot add %s (%s) for /dev/%s: cannot open %s: %s",
                devpath, devno, name, uevent, strerror(errno));
        return;
    }
    written = write(fd, "add\n", 4);
    if (written != 4)
        log_msg("coldboot add %s (%s) for /dev/%s failed at %s: %s",
                devpath, devno, name, uevent,
                written < 0 ? strerror(errno) : "short write");
    else
        log_msg("coldboot add emitted for %s (%s), /dev/%s", devpath, uevent,
                name);
    close(fd);
}

static int open_parent(const char *name, char *leaf, size_t leaf_size, bool create)
{
    char copy[FIELD_MAX];
    char *parts[FIELD_MAX / 2];
    size_t count = 0;
    int fd;

    if (strlen(name) >= sizeof(copy)) return -1;
    strcpy(copy, name);
    char *cursor = copy;
    while (cursor) {
        char *slash = strchr(cursor, '/');
        if (slash) *slash = '\0';
        if (count >= sizeof(parts) / sizeof(parts[0])) { errno = ENAMETOOLONG; return -1; }
        parts[count++] = cursor;
        if (!slash) break;
        cursor = slash + 1;
    }
    if (!count || strlen(parts[count - 1]) >= leaf_size) { errno = ENAMETOOLONG; return -1; }
    strcpy(leaf, parts[count - 1]);

    fd = fcntl(dev_fd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) return -1;
    for (size_t i = 0; i + 1 < count; i++) {
        struct stat st;
        int next;
        if (fstatat(fd, parts[i], &st, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno != ENOENT || !create || mkdirat(fd, parts[i], 0755) < 0) {
                close(fd); return -1;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR; close(fd); return -1;
        }
        next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) { close(fd); return -1; }
        close(fd);
        fd = next;
    }
    return fd;
}

static int ensure_node(const char *name, bool block, unsigned int major,
                       unsigned int minor, const char *origin, bool *created)
{
    char leaf[NAME_MAX + 1];
    struct stat st;
    int parent, result = -1;
    mode_t expected = (block ? S_IFBLK : S_IFCHR);
    dev_t device = makedev(major, minor);

    if (created) *created = false;

    if (!valid_name(name)) {
        log_msg("refusing unsafe DEVNAME '%s'", name ? name : "(null)");
        return -1;
    }
    parent = open_parent(name, leaf, sizeof(leaf), true);
    if (parent < 0) {
        log_msg("%s /dev/%s: cannot open parent: %s", origin, name, strerror(errno));
        return -1;
    }
    if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if ((st.st_mode & S_IFMT) == expected && st.st_rdev == device) {
            result = 0;
            goto adopted;
        }
        if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) {
            log_msg("%s /dev/%s: refusing to replace unrelated existing path", origin, name);
            goto out;
        }
        log_msg("%s /dev/%s: replacing wrong device node", origin, name);
        if (unlinkat(parent, leaf, 0) < 0) goto out;
        if (created) *created = true;
    } else if (errno != ENOENT) {
        log_msg("%s /dev/%s: cannot inspect path: %s", origin, name, strerror(errno));
        goto out;
    }
    int mknod_result = mknodat(parent, leaf, expected | 0600, device);
    if (mknod_result < 0 && errno != EEXIST) {
        log_msg("%s /dev/%s: mknod failed: %s", origin, name, strerror(errno));
        goto out;
    }
    if (created && mknod_result == 0) *created = true;
    if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
        (st.st_mode & S_IFMT) != expected || st.st_rdev != device) {
        log_msg("%s /dev/%s: node verification failed", origin, name);
        goto out;
    }
    result = 0;
adopted:
    if (result == 0) apply_dev_policy(name);
    if (result == 0 && !remember_managed(name, block, major, minor,
                                         created && *created)) result = -1;
out:
    close(parent);
    return result;
}

static int ensure_partition_alias(const char *partition_name, const char *devname,
                                  unsigned int major, unsigned int minor,
                                  const char *origin, bool *created)
{
    char alias[FIELD_MAX], target[FIELD_MAX], existing[FIELD_MAX];
    char leaf[NAME_MAX + 1];
    struct stat st;
    int parent;
    ssize_t n;
    (void)major;
    (void)minor;
    if (created) *created = false;

    if (!valid_partition_name(partition_name) || !valid_name(devname)) {
        log_msg("%s: refusing unsafe PARTNAME '%s' or DEVNAME '%s'", origin,
                partition_name ? partition_name : "(null)", devname ? devname : "(null)");
        return -1;
    }
    if (snprintf(alias, sizeof(alias), "block/by-name/%s", partition_name) >= (int)sizeof(alias) ||
        snprintf(target, sizeof(target), "/dev/%s", devname) >= (int)sizeof(target)) return -1;
    parent = open_parent(alias, leaf, sizeof(leaf), true);
    if (parent < 0) {
        log_msg("%s /dev/%s: cannot open parent: %s", origin, alias, strerror(errno));
        return -1;
    }
    if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            log_msg("%s /dev/%s: refusing to replace unrelated non-symlink", origin, alias);
            close(parent);
            return -1;
        }
        n = readlinkat(parent, leaf, existing, sizeof(existing) - 1);
        if (n >= 0) {
            existing[n] = '\0';
            if (!strcmp(existing, target)) { close(parent); return 0; }
        }
        log_msg("%s /dev/%s: conflicting existing symlink", origin, alias);
        close(parent);
        return -1;
    }
    if (errno != ENOENT) {
        log_msg("%s /dev/%s: cannot inspect alias: %s", origin, alias, strerror(errno));
        close(parent);
        return -1;
    }
    if (symlinkat(target, parent, leaf) < 0 && errno != EEXIST) {
        log_msg("%s /dev/%s: symlink failed: %s", origin, alias, strerror(errno));
        close(parent);
        return -1;
    }
    if (created) *created = true;
    if (readlinkat(parent, leaf, existing, sizeof(existing) - 1) < 0) {
        log_msg("%s /dev/%s: symlink verification failed", origin, alias);
        close(parent);
        return -1;
    }
    close(parent);
    return 0;
}

static void remove_managed_alias(struct managed_node *n)
{
    char leaf[NAME_MAX + 1];
    struct stat st;
    int parent;
    ssize_t length;

    if (!n || !n->alias_owned || !n->alias || !n->alias_target) return;
    parent = open_parent(n->alias, leaf, sizeof(leaf), false);
    if (parent < 0) return;
    if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
        char existing[FIELD_MAX];
        length = readlinkat(parent, leaf, existing, sizeof(existing) - 1);
        if (length >= 0) {
            existing[length] = '\0';
            if (!strcmp(existing, n->alias_target)) (void)unlinkat(parent, leaf, 0);
        }
    }
    close(parent);
}

static bool read_fields(const char *path, char *action, char *devpath,
                        char *subsystem, char *devname, unsigned int *major,
                        unsigned int *minor, char *partition_name)
{
    FILE *file = fopen(path, "re");
    char line[FIELD_MAX + 32];
    bool have_major = false, have_minor = false;
    if (!file) return false;
    while (fgets(line, sizeof(line), file)) {
        char *eq = strchr(line, '=');
        char *value;
        if (!eq) continue;
        *eq = '\0'; value = eq + 1;
        value[strcspn(value, "\r\n")] = '\0';
        if (!strcmp(line, "ACTION")) snprintf(action, FIELD_MAX, "%s", value);
        else if (!strcmp(line, "DEVPATH")) snprintf(devpath, FIELD_MAX, "%s", value);
        else if (!strcmp(line, "SUBSYSTEM")) snprintf(subsystem, FIELD_MAX, "%s", value);
        else if (!strcmp(line, "DEVNAME")) snprintf(devname, FIELD_MAX, "%s", value);
        else if (!strcmp(line, "PARTNAME")) snprintf(partition_name, FIELD_MAX, "%s", value);
        else if (!strcmp(line, "MAJOR")) have_major = parse_uint(value, major);
        else if (!strcmp(line, "MINOR")) have_minor = parse_uint(value, minor);
    }
    fclose(file);
    return have_major && have_minor && devname[0] != '\0';
}

static void handle_event(const char *payload, size_t length)
{
    char action[FIELD_MAX] = "", devpath[FIELD_MAX] = "";
    char subsystem[FIELD_MAX] = "", devname[FIELD_MAX] = "";
    char partition_name[FIELD_MAX] = "";
    char firmware[FIELD_MAX] = "";
    unsigned int major = 0, minor = 0;
    bool have_major = false, have_minor = false;
    bool block;
    const char *p = payload, *end = payload + length;
    while (p < end && *p) {
        const char *eq = strchr(p, '=');
        size_t n = strnlen(p, (size_t)(end - p));
        if (eq && eq < p + n) {
            size_t vlen = n - (size_t)(eq - p) - 1;
            const char *value = eq + 1;
            size_t keylen = (size_t)(eq - p);
            if (keylen == 6 && !memcmp(p, "ACTION", 6)) snprintf(action, sizeof(action), "%.*s", (int)vlen, value);
            else if (keylen == 7 && !memcmp(p, "DEVPATH", 7)) snprintf(devpath, sizeof(devpath), "%.*s", (int)vlen, value);
            else if (keylen == 9 && !memcmp(p, "SUBSYSTEM", 9)) snprintf(subsystem, sizeof(subsystem), "%.*s", (int)vlen, value);
            else if (keylen == 7 && !memcmp(p, "DEVNAME", 7)) snprintf(devname, sizeof(devname), "%.*s", (int)vlen, value);
            else if (keylen == 8 && !memcmp(p, "PARTNAME", 8)) snprintf(partition_name, sizeof(partition_name), "%.*s", (int)vlen, value);
            else if (keylen == 8 && !memcmp(p, "FIRMWARE", 8)) snprintf(firmware, sizeof(firmware), "%.*s", (int)vlen, value);
            else if (keylen == 5 && !memcmp(p, "MAJOR", 5)) { char v[32]; snprintf(v, sizeof(v), "%.*s", (int)vlen, value); have_major = parse_uint(v, &major); }
            else if (keylen == 5 && !memcmp(p, "MINOR", 5)) { char v[32]; snprintf(v, sizeof(v), "%.*s", (int)vlen, value); have_minor = parse_uint(v, &minor); }
        }
        p += n + 1;
    }
    if (!strcmp(action, "add") && firmware[0]) firmware_event(devpath, firmware);
    if (!strcmp(action, "remove")) {
        const char *removed_name = shape_name(subsystem, devpath, devname);
        struct managed_node *n = removed_name ? find_managed(removed_name) : NULL;
        if (!n) return;
        /* The kernel multicast has already delivered this remove lifecycle to
         * udev.  Keep cleanup after handling the event, and never destroy an
         * existing node merely because devd adopted it during coldboot. */
        if (n->owned) {
            char leaf[NAME_MAX + 1];
            int parent = open_parent(n->name, leaf, sizeof(leaf), false);
            struct stat st;
            if (parent < 0) {
                log_msg("remove /dev/%s: cannot inspect parent: %s", n->name, strerror(errno));
                return;
            }
            if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                ((n->block && S_ISBLK(st.st_mode)) || (!n->block && S_ISCHR(st.st_mode))) &&
                st.st_rdev == makedev(n->major, n->minor)) {
                if (unlinkat(parent, leaf, 0) == 0 || errno == ENOENT)
                    log_msg("remove /dev/%s", n->name);
            } else if (errno != ENOENT) {
                log_msg("remove /dev/%s: pathname was replaced; leaving it alone", n->name);
            }
            close(parent);
        } else {
            log_msg("remove /dev/%s: adopted existing node; leaving it alone", n->name);
        }
        remove_managed_alias(n);
        forget_managed(n->name);
        return;
    }
    if (strcmp(action, "add") && strcmp(action, "change")) return;
    if (!valid_name(devname) || !have_major || !have_minor) { apply_sys_policy(devpath); return; }
    block = !strcmp(subsystem, "block");
    if (!block) {
        char index_path[PATH_MAX];
        snprintf(index_path, sizeof(index_path), "%s/dev/char/%u:%u", SYS_ROOT, major, minor);
        if (access(index_path, F_OK) != 0) return;
    }
    const char *final_name = shape_name(subsystem, devpath, devname);
    if (!final_name || !valid_name(final_name)) { log_msg("refusing unsafe shaped DEVNAME"); return; }
    if (!strcmp(action, "change")) {
        struct managed_node *n = find_managed(final_name);
        if (n) {
            if (n->block == block && n->major == major && n->minor == minor)
                apply_dev_policy(final_name);
            else
                log_msg("change /dev/%s: dev_t changed; leaving existing node alone", final_name);
        }
        apply_sys_policy(devpath);
        return;
    }
    bool node_created = false;
    if (ensure_node(final_name, block, major, minor, action, &node_created) == 0)
        log_msg("%s %s %u:%u -> /dev/%s", action, block ? "block" : "char", major, minor, final_name);
    if (block && partition_name[0]) {
        bool alias_created = false;
        if (ensure_partition_alias(partition_name, devname, major, minor, action,
                                   &alias_created) == 0) {
            struct managed_node *n = find_managed(final_name);
            char alias[FIELD_MAX], target[FIELD_MAX];
            if (n && snprintf(alias, sizeof(alias), "block/by-name/%s", partition_name) < (int)sizeof(alias) &&
                snprintf(target, sizeof(target), "/dev/%s", devname) < (int)sizeof(target))
                remember_alias(n, alias, target, alias_created);
            log_msg("%s partition %s -> /dev/block/by-name/%s", action, partition_name, partition_name);
        }
    }
    apply_sys_policy(devpath);
}

static void coldboot_index(const char *index, bool block)
{
    DIR *dir = opendir(index);
    struct dirent *entry;
    if (!dir) { log_msg("coldboot %s: cannot open %s: %s", block ? "block" : "char", index, strerror(errno)); return; }
    while (!stopping && (entry = readdir(dir))) {
        unsigned int major, minor; char path[PATH_MAX], name[FIELD_MAX];
        char action[FIELD_MAX] = "", devpath[FIELD_MAX] = "", subsystem[FIELD_MAX] = "", devname[FIELD_MAX] = "", partition_name[FIELD_MAX] = "";
        if (entry->d_name[0] == '.' || !parse_devno(entry->d_name, &major, &minor)) continue;
        snprintf(path, sizeof(path), "%s/%s/uevent", index, entry->d_name);
        if (!read_fields(path, action, devpath, subsystem, devname, &major, &minor, partition_name)) {
            log_msg("coldboot %s %s: missing usable uevent metadata", block ? "block" : "char", entry->d_name);
            continue;
        }
        const char *shaped = shape_name(subsystem, devpath, devname);
        if (!shaped || !valid_name(shaped)) { log_msg("coldboot: refusing unsafe shaped DEVNAME"); continue; }
        snprintf(name, sizeof(name), "%s", shaped);
        bool created = false;
        if (ensure_node(name, block, major, minor, "coldboot", &created) == 0)
            log_msg("coldboot %s %u:%u -> /dev/%s", block ? "block" : "char", major, minor, name);
        if (created)
            trigger_coldboot_add(index, entry->d_name, devpath, name);
        if (block && partition_name[0]) {
            bool alias_created = false;
            if (ensure_partition_alias(partition_name, devname, major, minor,
                                       "coldboot", &alias_created) == 0) {
                struct managed_node *n = find_managed(name);
                char alias[FIELD_MAX], target[FIELD_MAX];
                if (n && snprintf(alias, sizeof(alias), "block/by-name/%s", partition_name) < (int)sizeof(alias) &&
                    snprintf(target, sizeof(target), "/dev/%s", devname) < (int)sizeof(target))
                    remember_alias(n, alias, target, alias_created);
                log_msg("coldboot partition %s -> /dev/block/by-name/%s", partition_name, partition_name);
            }
        }
        apply_sys_policy(devpath);
    }
    closedir(dir);
}

int main(void)
{
    struct sockaddr_nl address = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    struct sigaction sa = { .sa_handler = on_signal };
    char payload[UDEV_PAYLOAD_MAX];
    int sock;

    if (access(DEV_ROOT, W_OK) || access(SYS_ROOT "/devices", R_OK) ||
        access(SYS_ROOT "/dev/char", R_OK) || access(SYS_ROOT "/dev/block", R_OK)) {
        log_msg("/dev or required /sys indexes are unavailable: %s", strerror(errno)); return EXIT_FAILURE;
    }
    dev_fd = open(DEV_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dev_fd < 0) { log_msg("cannot open /dev safely: %s", strerror(errno)); return EXIT_FAILURE; }
    sigemptyset(&sa.sa_mask); sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    sock = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (sock < 0 || bind(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        log_msg("cannot create/bind uevent socket: %s", strerror(errno)); if (sock >= 0) close(sock); close(dev_fd); return EXIT_FAILURE;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &tb_socket_rcvbuf, sizeof(tb_socket_rcvbuf)) < 0)
        log_msg("SO_RCVBUF %d failed: %s", tb_socket_rcvbuf, strerror(errno));
    log_msg("listening; starting coldboot");
    coldboot_index(SYS_ROOT "/dev/char", false);
    coldboot_index(SYS_ROOT "/dev/block", true);
    notify_ready();
    log_msg("ready after coldboot");
    while (!stopping) {
        ssize_t received = recv(sock, payload, sizeof(payload), 0);
        if (received < 0) { if (errno == EINTR) continue; log_msg("uevent receive failed: %s", strerror(errno)); break; }
        handle_event(payload, (size_t)received);
    }
    close(sock); close(dev_fd);
    while (managed) { struct managed_node *n = managed; managed = n->next; free(n->name); free(n->alias); free(n->alias_target); free(n); }
    log_msg("stopped");
    return EXIT_SUCCESS;
}
