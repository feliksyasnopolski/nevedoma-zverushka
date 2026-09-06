#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg) { int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r; }
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static void fail(const char *what) { fprintf(stderr, "consumer_error operation=%s errno=%d message=%s\n", what, errno, strerror(errno)); exit(2); }

int main(int argc, char **argv) {
  const char *node = argc > 1 ? argv[1] : "/dev/video-rear";
  unsigned duration = argc > 2 ? (unsigned)atoi(argv[2]) : 180;
  const unsigned width = 1920, height = 1080, expected_size = width * height * 3 / 2;
  int fd = open(node, O_RDWR | O_NONBLOCK); if (fd < 0) fail("open");
  printf("consumer_start pid=%ld fd=%d node=%s\n", (long)getpid(), fd, node);
  struct v4l2_format f = {0}; f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; f.fmt.pix.width = width; f.fmt.pix.height = height; f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420; f.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) fail("S_FMT");
  printf("format width=%u height=%u fourcc=%c%c%c%c bytesperline=%u sizeimage=%u\n", f.fmt.pix.width, f.fmt.pix.height, f.fmt.pix.pixelformat & 0xff, (f.fmt.pix.pixelformat >> 8) & 0xff, (f.fmt.pix.pixelformat >> 16) & 0xff, (f.fmt.pix.pixelformat >> 24) & 0xff, f.fmt.pix.bytesperline, f.fmt.pix.sizeimage);
  if (f.fmt.pix.width != width || f.fmt.pix.height != height || f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420 || f.fmt.pix.sizeimage != expected_size) { fprintf(stderr, "consumer_error operation=format_contract expected=%u\n", expected_size); return 3; }
  struct v4l2_requestbuffers r = {0}; r.count = 4; r.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; r.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &r) < 0 || r.count < 1) fail("REQBUFS");
  void *maps[8] = {0}; unsigned lengths[8] = {0};
  for (unsigned i = 0; i < r.count; ++i) { struct v4l2_buffer b = {0}; b.type = r.type; b.memory = r.memory; b.index = i; if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) fail("QUERYBUF"); maps[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset); if (maps[i] == MAP_FAILED) fail("mmap"); lengths[i] = b.length; if (xioctl(fd, VIDIOC_QBUF, &b) < 0) fail("QBUF_setup"); }
  struct v4l2_control keep = {.id = V4L2_CID_USER_BASE + 0xf000, .value = 1}; if (xioctl(fd, VIDIOC_S_CTRL, &keep) < 0) fail("S_CTRL_keep_format");
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) fail("STREAMON"); printf("streamon buffers=%u expected_frame_size=%u\n", r.count, expected_size); fflush(stdout);
  uint64_t start = now_ns(), last = 0, max_gap = 0, frames = 0; unsigned timeouts = 0, dq_errors = 0, q_errors = 0;
  while (now_ns() - start < (uint64_t)duration * 1000000000ull) { struct pollfd p = {.fd = fd, .events = POLLIN | POLLERR}; int pr = poll(&p, 1, 1000); if (pr == 0) { ++timeouts; printf("timeout t_ns=%" PRIu64 "\n", now_ns() - start); fflush(stdout); continue; } if (pr < 0) { if (errno == EINTR) continue; fail("poll"); } struct v4l2_buffer b = {0}; b.type = type; b.memory = V4L2_MEMORY_MMAP; uint64_t t = now_ns(); if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { ++dq_errors; printf("dq_error t_ns=%" PRIu64 " errno=%d message=%s\n", t - start, errno, strerror(errno)); fflush(stdout); continue; } uint64_t gap = last ? t - last : 0; if (gap > max_gap) max_gap = gap; last = t; ++frames; printf("frame n=%" PRIu64 " t_ns=%" PRIu64 " gap_ns=%" PRIu64 " bytesused=%u index=%u\n", frames, t - start, gap, b.bytesused, b.index); if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { ++q_errors; printf("q_error t_ns=%" PRIu64 " errno=%d message=%s\n", now_ns() - start, errno, strerror(errno)); } fflush(stdout); }
  if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) fail("STREAMOFF_final"); for (unsigned i = 0; i < r.count; ++i) if (maps[i]) munmap(maps[i], lengths[i]); printf("summary frames=%" PRIu64 " max_gap_ns=%" PRIu64 " timeouts=%u dq_errors=%u q_errors=%u\n", frames, max_gap, timeouts, dq_errors, q_errors); close(fd); return (dq_errors || q_errors || frames == 0) ? 4 : 0;
}
