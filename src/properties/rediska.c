#define _GNU_SOURCE
#include "rediska_protocol.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROPS 8192
#define MAX_LINE 1024
enum log_level { LOG_NORMAL, LOG_TRACE };
struct entry {
  char name[REDISKA_NAME_MAX], value[REDISKA_VALUE_MAX];
  uint32_t serial;
};
struct watcher {
  int fd;
  pid_t pid;
  char prefix[REDISKA_NAME_MAX];
  struct watcher *next;
};
static struct entry props[MAX_PROPS];
static size_t prop_count;
static uint32_t global_serial = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER,
                       watch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct watcher *watchers;
static uint64_t counters[8], started_ns;
static enum log_level level = LOG_NORMAL;
static uint64_t mono_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static void quote(char *o, size_t n, const char *s) {
  size_t j = 0;
  if (!n)
    return;
  o[j++] = '"';
  while (*s && j + 2 < n) {
    unsigned char c = (unsigned char)*s++;
    if (c == '"' || c == '\\')
      o[j++] = '\\';
    if (c == '\n' || c == '\r' || c == '\t') {
      o[j++] = '\\';
      o[j++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
    } else if (c >= 32)
      o[j++] = (char)c;
  }
  if (j + 1 < n)
    o[j++] = '"';
  o[j] = 0;
}
static int send_full(int fd, const void *buf, size_t n, int flags) {
  const char *p = buf;
  while (n) {
    ssize_t r = send(fd, p, n, flags | MSG_NOSIGNAL);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (!r)
      return -1;
    p += r;
    n -= r;
  }
  return 0;
}
static int read_full(int fd, void *buf, size_t n) {
  char *p = buf;
  while (n) {
    ssize_t r = read(fd, p, n);
    if (!r)
      return -1;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += r;
    n -= r;
  }
  return 0;
}
static int find_index(const char *n) {
  for (size_t i = 0; i < prop_count; i++)
    if (!strcmp(props[i].name, n))
      return (int)i;
  return -1;
}
static int valid_string(const char *s, size_t n) {
  return memchr(s, 0, n) != NULL;
}
static pid_t peer_pid(int fd) {
#ifdef SO_PEERCRED
  struct ucred c;
  socklen_t n = sizeof c;
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c, &n) == 0 ? c.pid : 0;
#else
  (void)fd;
  return 0;
#endif
}
static void event_emit(const char *op, pid_t pid, const char *name,
                       const char *fmt, ...) {
  char d[768], line[1024], pn[260];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(d, sizeof d, fmt, ap);
  va_end(ap);
  quote(pn, sizeof pn, name ? name : "");
  int len =
      snprintf(line, sizeof line, "%.3f pid=%ld op=%s property=%s %s\n",
               (double)(mono_ns() - started_ns) / 1e9, (long)pid, op, pn, d);
  if (len < 0)
    return;
  if (level == LOG_TRACE || (strcmp(op, "GET") && strcmp(op, "FIND") &&
                             strcmp(op, "READ") && strcmp(op, "HELLO"))) {
    fputs(line, stderr);
    fflush(stderr);
  }
  pthread_mutex_lock(&watch_lock);
  struct watcher **pp = &watchers;
  while (*pp) {
    struct watcher *w = *pp;
    if ((!w->pid || w->pid == pid) &&
        (!w->prefix[0] ||
         (name && !strncmp(name, w->prefix, strlen(w->prefix))))) {
      if (send_full(w->fd, line, (size_t)len, MSG_DONTWAIT) < 0 &&
          errno != EINTR) {
        *pp = w->next;
        close(w->fd);
        free(w);
        continue;
      }
    }
    pp = &w->next;
  }
  pthread_mutex_unlock(&watch_lock);
}
static void count_op(uint32_t op) {
  if (op >= REDISKA_GET && op <= REDISKA_WAIT_ANY)
    counters[op - REDISKA_GET]++;
}
static void import_file(const char *path) {
  FILE *f = fopen(path, "r");
  char line[MAX_LINE];
  if (!f) {
    fprintf(stderr, "rediska: cannot import %s: %s\n", path, strerror(errno));
    return;
  }
  size_t imported = 0;
  while (fgets(line, sizeof line, f)) {
    char *n = line, *v, *p;
    while (*n == ' ' || *n == '\t')
      n++;
    if (!*n || *n == '#')
      continue;
    if (*n == '[') {
      p = strstr(n, "]: [");
      if (!p)
        continue;
      *p = 0;
      v = p + 4;
      p = strrchr(v, ']');
      if (!p)
        continue;
      *p = 0;
      n++;
    } else {
      p = strchr(n, '=');
      if (!p)
        continue;
      *p++ = 0;
      v = p;
    }
    while (*n == ' ' || *n == '\t')
      n++;
    char *e = n + strlen(n);
    while (e > n && (e[-1] == ' ' || e[-1] == '\t'))
      *--e = 0;
    while (*v == ' ' || *v == '\t')
      v++;
    e = v + strlen(v);
    while (e > v &&
           (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
      *--e = 0;
    if (!*n || strlen(n) >= REDISKA_NAME_MAX || strlen(v) >= REDISKA_VALUE_MAX)
      continue;
    int ix = find_index(n);
    if (ix < 0 && prop_count < MAX_PROPS) {
      ix = (int)prop_count++;
      strncpy(props[ix].name, n, sizeof props[ix].name - 1);
      imported++;
    }
    if (ix >= 0)
      strncpy(props[ix].value, v, sizeof props[ix].value - 1);
  }
  fclose(f);
  fprintf(stderr, "rediska: imported %s: %zu properties\n", path, imported);
}
static void response(const struct rediska_header *q, int status, const void *b,
                     uint32_t len, int fd) {
  struct rediska_header r = *q;
  r.magic = REDISKA_MAGIC;
  r.version = REDISKA_VERSION;
  r.opcode = (uint32_t)status;
  r.payload_len = len;
  send_full(fd, &r, sizeof r, 0);
  if (len)
    send_full(fd, b, len, 0);
}
static int forward_android_set(const char *n, const char *v) {
  const char *path = getenv("ANDROID_PROPERTY_SOCKET");
  if (!path || !*path) path = "/dev/socket/property_service";
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof a.sun_path) { close(fd); return -1; }
  strncpy(a.sun_path, path, sizeof a.sun_path - 1);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
  uint32_t command = 0x00020001u, name_len = (uint32_t)strlen(n), value_len = (uint32_t)strlen(v), result = 0xffffffffu;
  int ok = send_full(fd, &command, sizeof command, 0) == 0 &&
           send_full(fd, &name_len, sizeof name_len, 0) == 0 &&
           send_full(fd, n, name_len, 0) == 0 &&
           send_full(fd, &value_len, sizeof value_len, 0) == 0 &&
           send_full(fd, v, value_len, 0) == 0 &&
           read_full(fd, &result, sizeof result) == 0;
  close(fd);
  return ok && result == 0 ? 0 : -1;
}
static int do_set(const char *n, const char *v, pid_t pid, uint32_t *out, int mirror) {
  /* Android read-only properties may be established by bootstrap import, but
   * never changed through a runtime property client. */
  if (!strncmp(n, "ro.", 3))
    return -1;
  /* The genuine Android mmap area is authoritative. Forward ordinary
   * Rediska writes through its existing property-service protocol; the
   * resulting ANDROID_EVENT is reflected back with mirror=0 below. */
  if (mirror && forward_android_set(n, v) != 0)
    return -1;
  pthread_mutex_lock(&lock);
  int ix = find_index(n);
  if (ix < 0) {
    if (prop_count == MAX_PROPS) {
      pthread_mutex_unlock(&lock);
      return -1;
    }
    ix = (int)prop_count++;
    strncpy(props[ix].name, n, sizeof props[ix].name - 1);
  }
  char old[REDISKA_VALUE_MAX];
  strncpy(old, props[ix].value, sizeof old - 1);
  old[sizeof old - 1] = 0;
  int same = !strcmp(old, v);
  if (!same) {
    strncpy(props[ix].value, v, sizeof props[ix].value - 1);
    props[ix].value[sizeof props[ix].value - 1] = 0;
    props[ix].serial = ++global_serial;
    pthread_cond_broadcast(&changed);
  }
  uint32_t s = props[ix].serial, g = global_serial;
  pthread_mutex_unlock(&lock);
  if (out)
    *out = s;
  char qo[220], qn[220];
  quote(qo, sizeof qo, old);
  quote(qn, sizeof qn, v);
  event_emit("SET", pid, n, "old=%s new=%s changed=%s serial=%u global=%u", qo,
             qn, same ? "no" : "yes", s, g);
  return 0;
}
static void *property_client(void *arg) {
  int fd = (int)(intptr_t)arg;
  pid_t pid = peer_pid(fd);
  struct rediska_header q;
  struct rediska_request in = {0};
  struct rediska_property out = {0};
  if (read_full(fd, &q, sizeof q) < 0 || q.magic != REDISKA_MAGIC ||
      q.version != REDISKA_VERSION || q.payload_len != sizeof in) {
    counters[7]++;
    close(fd);
    event_emit("ERROR", pid, "", "status=malformed_request");
    return NULL;
  }
  if (read_full(fd, &in, sizeof in) < 0) {
    close(fd);
    return NULL;
  }
  if (!valid_string(in.name, sizeof in.name) ||
      !valid_string(in.value, sizeof in.value)) {
    counters[7]++;
    response(&q, REDISKA_BAD_REQUEST, NULL, 0, fd);
    close(fd);
    event_emit("ERROR", pid, in.name, "status=malformed_request");
    return NULL;
  }
  count_op(q.opcode);
  const char *op = q.opcode == 1   ? "HELLO"
                   : q.opcode == 2 ? "GET"
                   : q.opcode == 3 ? "SET"
                   : q.opcode == 4 ? "FIND"
                   : q.opcode == 5 ? "READ"
                   : q.opcode == 6 ? "WAIT"
                   : q.opcode == 7 ? "WAIT_ANY"
                                   : "ERROR";
  event_emit(op, pid, in.name, "status=request");
  if (q.opcode == REDISKA_HELLO) {
    pthread_mutex_lock(&lock);
    uint32_t s = global_serial;
    pthread_mutex_unlock(&lock);
    response(&q, REDISKA_OK, &s, sizeof s, fd);
    close(fd);
    return NULL;
  }
  pthread_mutex_lock(&lock);
  int ix = find_index(in.name);
  if (q.opcode == REDISKA_GET || q.opcode == REDISKA_FIND ||
      q.opcode == REDISKA_READ) {
    if (ix < 0) {
      pthread_mutex_unlock(&lock);
      response(&q, REDISKA_NOT_FOUND, NULL, 0, fd);
      event_emit(op, pid, in.name, "status=NOT_FOUND");
    } else {
      memcpy(out.name, props[ix].name, sizeof out.name);
      memcpy(out.value, props[ix].value, sizeof out.value);
      out.serial = props[ix].serial;
      pthread_mutex_unlock(&lock);
      response(&q, REDISKA_OK, &out, sizeof out, fd);
      char v[220];
      quote(v, sizeof v, out.value);
      event_emit(op, pid, in.name, "value=%s serial=%u", v, out.serial);
    }
    close(fd);
    return NULL;
  }
  if (q.opcode == REDISKA_SET) {
    pthread_mutex_unlock(&lock);
    if (!*in.name || strlen(in.name) >= REDISKA_NAME_MAX ||
        strlen(in.value) >= REDISKA_VALUE_MAX) {
      counters[7]++;
      response(&q, REDISKA_BAD_REQUEST, NULL, 0, fd);
    } else {
      uint32_t s;
      if (do_set(in.name, in.value, pid, &s, 1) == 0)
        response(&q, REDISKA_OK, &s, sizeof s, fd);
      else
        response(&q, REDISKA_INTERNAL, NULL, 0, fd);
    }
    close(fd);
    return NULL;
  }
  if (q.opcode == REDISKA_WAIT || q.opcode == REDISKA_WAIT_ANY) {
    uint64_t begin = mono_ns();
    uint32_t start = in.serial;
    int timed = in.timeout_ns != UINT64_MAX;
    struct timespec until;
    if (timed) {
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += (time_t)(in.timeout_ns / 1000000000ull);
      until.tv_nsec += (long)(in.timeout_ns % 1000000000ull);
      if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
      }
    }
    event_emit("WAIT", pid, in.name, "requested=%u timeout_ns=%" PRIu64, start,
               in.timeout_ns);
    for (;;) {
      ix = find_index(in.name);
      uint32_t now = q.opcode == REDISKA_WAIT_ANY
                         ? global_serial
                         : (ix < 0 ? 0 : props[ix].serial);
      if (now != start) {
        pthread_mutex_unlock(&lock);
        response(&q, REDISKA_OK, &now, sizeof now, fd);
        event_emit("WAKE", pid, in.name, "serial=%u->%u elapsed_ms=%.3f", start,
                   now, (double)(mono_ns() - begin) / 1e6);
        close(fd);
        return NULL;
      }
      int e = timed ? pthread_cond_timedwait(&changed, &lock, &until)
                    : pthread_cond_wait(&changed, &lock);
      if (e == ETIMEDOUT) {
        pthread_mutex_unlock(&lock);
        counters[6]++;
        response(&q, REDISKA_TIMEOUT, NULL, 0, fd);
        event_emit("TIMEOUT", pid, in.name, "serial=%u elapsed_ms=%.3f", start,
                   (double)(mono_ns() - begin) / 1e6);
        close(fd);
        return NULL;
      }
    }
  }
  pthread_mutex_unlock(&lock);
  counters[7]++;
  response(&q, REDISKA_BAD_REQUEST, NULL, 0, fd);
  event_emit("ERROR", pid, in.name, "status=unsupported_opcode opcode=%u",
             q.opcode);
  close(fd);
  return NULL;
}
static void admin_reply(int fd, const char *fmt, ...) {
  char b[4096];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0)
    send_full(fd, b, (size_t)n, 0);
}
static int entry_cmp(const void *a, const void *b) {
  const struct entry *x = a, *y = b;
  return strcmp(x->name, y->name);
}
static void admin_dump(int fd) {
  pthread_mutex_lock(&lock);
  qsort(props, prop_count, sizeof props[0], entry_cmp);
  for (size_t i = 0; i < prop_count; i++)
    admin_reply(fd, "%s=%s\n", props[i].name, props[i].value);
  pthread_mutex_unlock(&lock);
  admin_reply(fd, "END\n");
}
static void admin_stats(int fd) {
  pthread_mutex_lock(&lock);
  size_t pc = prop_count;
  uint32_t gs = global_serial;
  pthread_mutex_unlock(&lock);
  size_t wc = 0;
  pthread_mutex_lock(&watch_lock);
  for (struct watcher *w = watchers; w; w = w->next)
    wc++;
  pthread_mutex_unlock(&watch_lock);
  admin_reply(fd,
              "properties=%zu\nglobal_serial=%u\nwatchers=%zu\nuptime_ms=%."
              "0f\nGET=%" PRIu64 "\nSET=%" PRIu64 "\nFIND=%" PRIu64
              "\nREAD=%" PRIu64 "\nWAIT=%" PRIu64 "\nWAIT_ANY=%" PRIu64
              "\ntimeouts=%" PRIu64 "\nerrors=%" PRIu64 "\nEND\n",
              pc, gs, wc, (double)(mono_ns() - started_ns) / 1e6, counters[0],
              counters[1], counters[2], counters[3], counters[4], counters[5],
              counters[6], counters[7]);
}
static void *admin_client(void *arg) {
  int fd = (int)(intptr_t)arg;
  char line[MAX_LINE];
  ssize_t n = recv(fd, line, sizeof line - 1, 0);
  if (n <= 0) {
    close(fd);
    return NULL;
  }
  line[n] = 0;
  char raw[MAX_LINE];
  memcpy(raw, line, (size_t)n + 1);
  char *cmd = strtok(line, " \t\r\n"), *a = strtok(NULL, " \t\r\n"),
       *b = strtok(NULL, "\r\n");
  if (!cmd) {
    close(fd);
    return NULL;
  }
  if (!strcmp(cmd, "ANDROID_EVENT")) {
    char *name = strstr(raw, " name=");
    char *value = strstr(raw, " value=");
    if (name) {
      name += 6;
      if (value)
        *value = 0;
    }
    if (value)
      value += 7;
    if (value) {
      char *end = value + strlen(value);
      while (end > value && (end[-1] == '\r' || end[-1] == '\n'))
        *--end = 0;
    }
    char *op = strstr(raw, " op=");
    char *result = strstr(raw, " result=");
    if (op)
      op += 4;
    if (result)
      result += 8;
    uint32_t rc = result ? (uint32_t)strtoul(result, NULL, 10) : 3;
    uint32_t old = 0, newer = 0;
    char *oldp = strstr(raw, " old=");
    char *newp = strstr(raw, " new=");
    if (oldp)
      old = (uint32_t)strtoul(oldp + 5, NULL, 10);
    if (newp)
      newer = (uint32_t)strtoul(newp + 5, NULL, 10);
    pid_t pid = 0;
    char *pp = strstr(raw, "pid=");
    if (pp)
      pid = (pid_t)strtol(pp + 4, NULL, 10);
    if (!name)
      name = (char *)"";
    if (!value)
      value = (char *)"";
    event_emit(op && rc == 0
                   ? (strcmp(op, "ADD") ? "ANDROID_UPDATE" : "ANDROID_ADD")
                   : "ANDROID_REJECT",
               pid, name,
               "uid_gid_result=%u result=%u old_serial=%u new_serial=%u "
               "requested_value=%s",
               rc, rc, old, newer, value);
    if (rc == 0) {
      /* The genuine Android area is authoritative for Android writes. Keep
       * the existing broker's logical view synchronized for legacy clients. */
      uint32_t ignored;
      (void)do_set(name, value, pid, &ignored, 0);
      counters[1]++;
    }
    admin_reply(fd, "OK\n");
    close(fd);
    return NULL;
  }
  if (!strcmp(cmd, "GET") || !strcmp(cmd, "FIND")) {
    pthread_mutex_lock(&lock);
    int ix = a ? find_index(a) : -1;
    if (ix < 0) {
      pthread_mutex_unlock(&lock);
      admin_reply(fd, "NOT_FOUND\n");
    } else {
      admin_reply(fd, "OK %s %u %u\n", props[ix].value, props[ix].serial,
                  global_serial);
      pthread_mutex_unlock(&lock);
    }
    close(fd);
    return NULL;
  }
  if (!strcmp(cmd, "SET") && a && b) {
    while (*b == ' ' || *b == '\t')
      b++;
    uint32_t s;
    if (do_set(a, b, peer_pid(fd), &s, 1) == 0)
      admin_reply(fd, "OK %u\n", s);
    else
      admin_reply(fd, "ERROR\n");
    close(fd);
    return NULL;
  }
  if (!strcmp(cmd, "DUMP")) {
    admin_dump(fd);
    close(fd);
    return NULL;
  }
  if (!strcmp(cmd, "STATS")) {
    admin_stats(fd);
    close(fd);
    return NULL;
  }
  if (!strcmp(cmd, "WATCH")) {
    struct watcher *w = calloc(1, sizeof *w);
    if (!w) {
      close(fd);
      return NULL;
    }
    w->fd = fd;
    for (char *x = a; x; x = strtok(NULL, " \t\r\n")) {
      if (!strncmp(x, "pid=", 4))
        w->pid = (pid_t)strtol(x + 4, NULL, 10);
      else if (!strncmp(x, "prefix=", 7))
        strncpy(w->prefix, x + 7, sizeof w->prefix - 1);
    }
    pthread_mutex_lock(&watch_lock);
    w->next = watchers;
    watchers = w;
    pthread_mutex_unlock(&watch_lock);
    admin_reply(fd, "OK WATCHING\n");
    while (recv(fd, line, sizeof line, 0) > 0) {
    }
    pthread_mutex_lock(&watch_lock);
    struct watcher **pp = &watchers;
    while (*pp && *pp != w)
      pp = &(*pp)->next;
    if (*pp)
      *pp = w->next;
    pthread_mutex_unlock(&watch_lock);
    free(w);
    close(fd);
    return NULL;
  }
  admin_reply(fd, "ERROR unknown_command\n");
  close(fd);
  return NULL;
}
static int make_socket(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof a.sun_path) {
    close(fd);
    return -1;
  }
  strncpy(a.sun_path, path, sizeof a.sun_path - 1);
  unlink(path);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 64) < 0) {
    close(fd);
    return -1;
  }
  chmod(path, 0660);
  return fd;
}
int main(int argc, char **argv) {
  const char *sock = getenv("TB330FU_REDISKA_SOCKET"),
             *file = getenv("FAKEPROPS_FILE"),
             *admin = getenv("TB330FU_REDISKA_ADMIN_SOCKET");
  if (!sock || !*sock)
    sock = REDISKA_SOCKET_DEFAULT;
  if (!file || !*file)
    file = "/var/lib/tb330fu/binder/android-properties.txt";
  if (!admin || !*admin)
    admin = "/run/tb330fu/rediska-admin.sock";
  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], "--log-level=", 12))
      level = !strcmp(argv[i] + 12, "trace") ? LOG_TRACE : LOG_NORMAL;
  started_ns = mono_ns();
  char dir[sizeof(((struct sockaddr_un *)0)->sun_path)];
  strncpy(dir, sock, sizeof dir - 1);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    mkdir(dir, 0755);
  }
  int fd = make_socket(sock);
  if (fd < 0)
    return 1;
  char adir[sizeof dir];
  strncpy(adir, admin, sizeof adir - 1);
  slash = strrchr(adir, '/');
  if (slash) {
    *slash = 0;
    mkdir(adir, 0755);
  }
  int afd = make_socket(admin);
  if (afd < 0)
    return 1;
  pthread_mutex_lock(&lock);
  import_file(file);
  pthread_mutex_unlock(&lock);
  signal(SIGPIPE, SIG_IGN);
  fprintf(stderr, "rediska: listening socket=%s admin=%s log_level=%s\n", sock,
          admin, level == LOG_TRACE ? "trace" : "normal");
  for (;;) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    FD_SET(afd, &rf);
    int max = fd > afd ? fd : afd;
    if (select(max + 1, &rf, NULL, NULL, NULL) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    int c;
    if (FD_ISSET(fd, &rf)) {
      c = accept(fd, NULL, NULL);
      if (c >= 0) {
        pthread_t t;
        if (!pthread_create(&t, NULL, property_client, (void *)(intptr_t)c))
          pthread_detach(t);
        else
          close(c);
      }
    }
    if (FD_ISSET(afd, &rf)) {
      c = accept(afd, NULL, NULL);
      if (c >= 0) {
        pthread_t t;
        if (!pthread_create(&t, NULL, admin_client, (void *)(intptr_t)c))
          pthread_detach(t);
        else
          close(c);
      }
    }
  }
  return 1;
}
