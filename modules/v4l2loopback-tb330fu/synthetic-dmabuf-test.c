#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg) { int r; do r=ioctl(fd,req,arg); while(r<0 && errno==EINTR); return r; }
static void die(const char *s) { perror(s); exit(1); }
int main(void) {
    const unsigned w=640,h=480,n=w*h*3/2;
    int dh=open("/dev/dma_heap/system",O_RDWR), out=open("/dev/video10",O_RDWR), cap=open("/dev/video10",O_RDWR);
    if(dh<0||out<0||cap<0) die("open");
    struct dma_heap_allocation_data a={.len=n,.fd_flags=O_RDWR|O_CLOEXEC};
    if(xioctl(dh,DMA_HEAP_IOCTL_ALLOC,&a)<0) die("dma_heap_alloc");
    uint8_t *src=mmap(NULL,n,PROT_READ|PROT_WRITE,MAP_SHARED,a.fd,0); if(src==MAP_FAILED) die("mmap dma");
    for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) src[y*w+x]=(uint8_t)(x*255/w);
    memset(src+w*h,128,n-w*h);
    struct v4l2_format f={.type=V4L2_BUF_TYPE_VIDEO_OUTPUT}; f.fmt.pix.width=w; f.fmt.pix.height=h; f.fmt.pix.pixelformat=V4L2_PIX_FMT_NV21; f.fmt.pix.field=V4L2_FIELD_NONE; f.fmt.pix.bytesperline=w; f.fmt.pix.sizeimage=n;
    if(xioctl(out,VIDIOC_S_FMT,&f)<0) die("S_FMT out");
    struct v4l2_requestbuffers ro={.count=1,.type=V4L2_BUF_TYPE_VIDEO_OUTPUT,.memory=V4L2_MEMORY_DMABUF}; if(xioctl(out,VIDIOC_REQBUFS,&ro)<0) die("REQBUFS out"); printf("REQBUFS_OUTPUT count=%u caps=0x%x\n",ro.count,ro.capabilities);
    f.type=V4L2_BUF_TYPE_VIDEO_CAPTURE; if(xioctl(cap,VIDIOC_S_FMT,&f)<0) die("S_FMT cap"); struct v4l2_requestbuffers rc={.count=2,.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,.memory=V4L2_MEMORY_MMAP}; if(xioctl(cap,VIDIOC_REQBUFS,&rc)<0) die("REQBUFS cap");
    struct v4l2_control keep={.id=V4L2_CID_USER_BASE+0xf000,.value=1}; if(xioctl(cap,VIDIOC_S_CTRL,&keep)<0) die("S_CTRL keep_format");
    void *maps[2]={0}; for(unsigned i=0;i<rc.count;i++){struct v4l2_buffer b={.type=rc.type,.memory=rc.memory,.index=i}; if(xioctl(cap,VIDIOC_QUERYBUF,&b)<0)die("QUERYBUF"); maps[i]=mmap(NULL,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,cap,b.m.offset); if(maps[i]==MAP_FAILED)die("mmap cap"); if(xioctl(cap,VIDIOC_QBUF,&b)<0)die("QBUF cap");}
    enum v4l2_buf_type t=V4L2_BUF_TYPE_VIDEO_CAPTURE; if(xioctl(cap,VIDIOC_STREAMON,&t)<0)die("STREAMON cap"); t=V4L2_BUF_TYPE_VIDEO_OUTPUT; if(xioctl(out,VIDIOC_STREAMON,&t)<0)die("STREAMON out");
    struct v4l2_buffer b={.type=t,.memory=V4L2_MEMORY_DMABUF,.index=0,.bytesused=n}; b.m.fd=a.fd; if(xioctl(out,VIDIOC_QBUF,&b)<0)die("QBUF dmabuf"); printf("QBUF_OUTPUT bytesused=%u\n",b.bytesused); if(xioctl(out,VIDIOC_DQBUF,&b)<0)die("DQBUF out"); printf("DQBUF_OUTPUT index=%u bytesused=%u\n",b.index,b.bytesused);
    t=V4L2_BUF_TYPE_VIDEO_CAPTURE; b=(struct v4l2_buffer){.type=t,.memory=V4L2_MEMORY_MMAP}; if(xioctl(cap,VIDIOC_DQBUF,&b)<0)die("DQBUF cap"); printf("DQBUF_CAPTURE index=%u bytesused=%u first=%u middle=%u\n",b.index,b.bytesused,((uint8_t*)maps[b.index])[0],((uint8_t*)maps[b.index])[w*h]);
    t=V4L2_BUF_TYPE_VIDEO_OUTPUT; xioctl(out,VIDIOC_STREAMOFF,&t); t=V4L2_BUF_TYPE_VIDEO_CAPTURE; xioctl(cap,VIDIOC_STREAMOFF,&t); munmap(src,n); close(a.fd); return 0;
}
