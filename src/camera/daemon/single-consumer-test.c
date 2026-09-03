#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg) {
  int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r;
}
int main(int argc, char **argv) {
  const char *node = argc > 1 ? argv[1] : "/dev/video10";
  unsigned width = argc > 4 ? (unsigned)atoi(argv[3]) : 640;
  unsigned height = argc > 4 ? (unsigned)atoi(argv[4]) : 480;
  int fd = open(node, O_RDWR | O_NONBLOCK); if (fd < 0) { perror("open"); return 2; }
  struct v4l2_format f = {0}; f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  f.fmt.pix.width=width; f.fmt.pix.height=height; f.fmt.pix.pixelformat=V4L2_PIX_FMT_NV21; f.fmt.pix.field=V4L2_FIELD_NONE;
  if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return 3; }
  struct v4l2_requestbuffers r = {0}; r.count=2; r.type=V4L2_BUF_TYPE_VIDEO_CAPTURE; r.memory=V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &r) < 0 || r.count < 1) { perror("REQBUFS"); return 4; }
  void *maps[4] = {0};
  for (unsigned i=0; i<r.count; ++i) {
    struct v4l2_buffer b = {0}; b.type=r.type; b.memory=r.memory; b.index=i;
    if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 5; }
    maps[i]=mmap(NULL,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.offset); if (maps[i]==MAP_FAILED) { perror("mmap"); return 6; }
    if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 6; }
  }
  struct v4l2_control keep = { .id=V4L2_CID_USER_BASE+0xf000, .value=1 };
  if (xioctl(fd, VIDIOC_S_CTRL, &keep) < 0) { perror("S_CTRL keep_format"); return 6; }
  enum v4l2_buf_type type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 7; }
  struct v4l2_buffer b = {0}; b.type=type; b.memory=V4L2_MEMORY_MMAP;
  for (int tries=0; tries<100; ++tries) { if (xioctl(fd,VIDIOC_DQBUF,&b)==0) { printf("capture_frame index=%u bytesused=%u first=%u\n",b.index,b.bytesused,((unsigned char*)maps[b.index])[0]); int hold=argc>2?atoi(argv[2]):0; if(hold) sleep(hold); type=V4L2_BUF_TYPE_VIDEO_CAPTURE; xioctl(fd,VIDIOC_STREAMOFF,&type); close(fd); return 0; } usleep(10000); }
  perror("DQBUF"); type=V4L2_BUF_TYPE_VIDEO_CAPTURE; xioctl(fd,VIDIOC_STREAMOFF,&type); close(fd); return 8;
}
