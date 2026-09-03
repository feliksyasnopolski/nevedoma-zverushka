#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define SOURCE_NAME "hall_irq"
#define VIRTUAL_NAME "TB330FU Folio Lid"
#define CLOSED_CODE 750
#define OPEN_CODE 751

static volatile sig_atomic_t stopping;

static void handle_signal(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static void log_message(const char *message) {
    fprintf(stderr, "tb330fu-folio-lid: %s\n", message);
}

static int open_source(void) {
    DIR *directory = opendir(INPUT_DIR);
    struct dirent *entry;

    if (!directory) {
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        char path[PATH_MAX];
        char name[256];
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", INPUT_DIR, entry->d_name) >=
            (int)sizeof(path)) {
            continue;
        }

        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            strcmp(name, SOURCE_NAME) == 0) {
            closedir(directory);
            fprintf(stderr, "tb330fu-folio-lid: source=%s\n", path);
            return fd;
        }
        close(fd);
    }

    closedir(directory);
    return -1;
}

static int create_uinput(void) {
    struct uinput_setup setup;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("tb330fu-folio-lid: open /dev/uinput");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_SW) < 0 ||
        ioctl(fd, UI_SET_SWBIT, SW_LID) < 0) {
        perror("tb330fu-folio-lid: configure uinput");
        close(fd);
        return -1;
    }

    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s", VIRTUAL_NAME);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1d6b;
    setup.id.product = 0x330f;
    setup.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("tb330fu-folio-lid: create uinput device");
        close(fd);
        return -1;
    }
    return fd;
}

static int emit_state(int uinput_fd, int state) {
    struct input_event event;
    memset(&event, 0, sizeof(event));
    gettimeofday(&event.time, NULL);
    event.type = EV_SW;
    event.code = SW_LID;
    event.value = state;
    if (write(uinput_fd, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
        return -1;
    }
    memset(&event, 0, sizeof(event));
    event.type = EV_SYN;
    event.code = SYN_REPORT;
    return write(uinput_fd, &event, sizeof(event)) == (ssize_t)sizeof(event) ? 0 : -1;
}

int main(void) {
    struct sigaction action;
    int source_fd = -1;
    int uinput_fd;
    int state = 0; /* Conservative startup state: open until the first edge. */

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    uinput_fd = create_uinput();
    if (uinput_fd < 0) {
        return EXIT_FAILURE;
    }
    log_message("virtual SW_LID device ready; startup state=open");

    while (!stopping) {
        struct pollfd pollfd;
        int poll_result;

        if (source_fd < 0) {
            source_fd = open_source();
            if (source_fd < 0) {
                struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };
                nanosleep(&delay, NULL);
                continue;
            }
        }

        pollfd.fd = source_fd;
        pollfd.events = POLLIN;
        pollfd.revents = 0;
        poll_result = poll(&pollfd, 1, 1000);
        if (poll_result < 0) {
            if (errno != EINTR) {
                close(source_fd);
                source_fd = -1;
            }
            continue;
        }
        if (poll_result == 0) {
            continue;
        }
        if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close(source_fd);
            source_fd = -1;
            log_message("source disappeared; rediscovering by name");
            continue;
        }

        for (;;) {
            struct input_event event;
            ssize_t count = read(source_fd, &event, sizeof(event));
            int next_state;
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
                break;
            }
            if (count <= 0) {
                close(source_fd);
                source_fd = -1;
                log_message("source read failed; rediscovering by name");
                break;
            }
            if (count != (ssize_t)sizeof(event)) {
                break;
            }
            if (event.type != EV_KEY || event.value != 1) {
                continue; /* Ignore release pulses and unrelated events. */
            }
            if (event.code == CLOSED_CODE) {
                next_state = 1;
            } else if (event.code == OPEN_CODE) {
                next_state = 0;
            } else {
                continue;
            }
            if (next_state == state) {
                continue;
            }
            if (emit_state(uinput_fd, next_state) < 0) {
                perror("tb330fu-folio-lid: emit SW_LID");
                stopping = 1;
                break;
            }
            state = next_state;
            fprintf(stderr, "tb330fu-folio-lid: folio=%s\n",
                    state ? "closed" : "open");
        }
    }

    if (source_fd >= 0) {
        close(source_fd);
    }
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    return EXIT_SUCCESS;
}
