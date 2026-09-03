#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

extern char **environ;

static void write_all(int fd, const char *s)
{
    const char *p = s;
    size_t left = 0;

    while (s[left] != '\0')
        left++;
    while (left != 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0)
            return;
        p += n;
        left -= (size_t)n;
    }
}

static void write_bytes(int fd, const char *p, size_t left)
{
    while (left != 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0)
            return;
        p += n;
        left -= (size_t)n;
    }
}

static void diagnostic(const char *s)
{
    int fd;

    write_all(STDERR_FILENO, s);
    fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        write_all(fd, s);
        close(fd);
    }
}

static void decimal_errno(int value)
{
    char buf[32];
    size_t pos = sizeof(buf);
    unsigned int n = value < 0 ? (unsigned int)-value : (unsigned int)value;

    buf[--pos] = '\n';
    do {
        buf[--pos] = (char)('0' + n % 10);
        n /= 10;
    } while (n != 0);
    if (value < 0)
        buf[--pos] = '-';
    write_bytes(STDERR_FILENO, buf + pos, sizeof(buf) - pos);
}

int main(void)
{
    char *const argv[] = {
        (char *)"/bin/busybox",
        (char *)"sh",
        (char *)"/etc/stage1.sh",
        NULL
    };
    int saved_errno;

    diagnostic("TB330FU INIT: exec BusyBox stage1\n");
    execve(argv[0], argv, environ);

    saved_errno = errno;
    diagnostic("TB330FU INIT: execve failed; errno follows: ");
    decimal_errno(saved_errno);
    diagnostic("TB330FU INIT: PID 1 halted after exec failure\n");
    for (;;)
        pause();
}
