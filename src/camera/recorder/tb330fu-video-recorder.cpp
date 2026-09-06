#include "../protocol/tb330fu_camera.pb.h"
#include "../video/tb330fu_video_ring.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace tb330fu::camera::v1;
namespace {
constexpr size_t kRingBytes = sizeof(tb330fu_video_ring_header) +
    TB330FU_VIDEO_RING_SLOTS * tb330fu_video_slot_bytes(TB330FU_VIDEO_RING_SLOT_BYTES);
constexpr uint8_t kMtkH264Avcc[] = {
    0x01, 0x42, 0x00, 0x0a, 0xff, 0xe1, 0x00, 0x14,
    0x67, 0x42, 0x00, 0x0a, 0x8d, 0x8d, 0x40, 0x3c,
    0x01, 0x13, 0xf2, 0xcd, 0x40, 0x40, 0x40, 0x41,
    0xe1, 0x10, 0x8d, 0x40, 0x01, 0x00, 0x04, 0x68,
    0xca, 0x43, 0xc8,
};

std::string av_error(int rc) { char b[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(rc, b, sizeof(b)); return b; }
void free_aligned(void *opaque, uint8_t *) { av_free(opaque); }
AVBufferRef *buffer(size_t size) {
    auto *raw = static_cast<uint8_t *>(av_malloc(size + 31));
    if (!raw) return nullptr;
    auto aligned = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(raw) + 31u) & ~uintptr_t(31u));
    auto *ref = av_buffer_create(aligned, size, free_aligned, raw, 0);
    if (!ref) av_free(raw);
    return ref;
}
bool is_h264_parameter_set_packet(const AVPacket *packet) {
    return packet && packet->size >= 8 && packet->data[0] == 0 && packet->data[1] == 0 &&
        packet->data[2] == 0 && packet->data[3] == 1 && packet->data[4] == 0x67;
}

class Recorder {
  public:
    ~Recorder() { stop(); }
    VideoStatus status() {
        std::lock_guard<std::mutex> lock(mu_);
        VideoStatus s = status_;
        s.set_bytes_written(part_bytes());
        if (header_) s.set_frames_transport_dropped(__atomic_load_n(&header_->frames_dropped, __ATOMIC_ACQUIRE));
        return s;
    }
    bool start(std::string *error) {
        std::lock_guard<std::mutex> lock(mu_);
        if (active_) { *error = "recording already active"; return false; }
        if (!prepare_ring(error) || !prepare_muxer(error)) { cleanup_locked(); return false; }
        status_.Clear(); status_.set_state(RECORDING_ACTIVE); active_ = true;
        worker_ = std::thread(&Recorder::consume, this);
        return true;
    }
    bool stop(std::string *error = nullptr) {
        std::thread worker;
        const bool was_active = active_.exchange(false, std::memory_order_release);
        { std::lock_guard<std::mutex> lock(mu_); if (!was_active && !worker_.joinable() && !format_ && !codec_) return true; worker = std::move(worker_); }
        if (worker.joinable()) worker.join();
        std::lock_guard<std::mutex> lock(mu_);
        int rc = status_.frames_encoded() == 0 ? AVERROR(EINVAL) : 0;
        if (rc >= 0) rc = av_write_trailer(format_);
        cleanup_ffmpeg_locked();
        if (rc < 0) { status_.set_state(RECORDING_ERROR); status_.set_last_error("finalization: " + av_error(rc)); if (error) *error = status_.last_error(); cleanup_locked(); return false; }
        if (::rename(TB330FU_VIDEO_OUTPUT_PART, TB330FU_VIDEO_OUTPUT) != 0) { status_.set_state(RECORDING_ERROR); status_.set_last_error(std::strerror(errno)); if (error) *error = status_.last_error(); cleanup_locked(); return false; }
        status_.set_state(RECORDING_COMPLETED); status_.set_path(TB330FU_VIDEO_OUTPUT); cleanup_locked(); return true;
    }
  private:
    bool prepare_ring(std::string *error) {
        fd_ = ::open(TB330FU_VIDEO_RING_PATH, O_RDWR | O_CREAT | O_TRUNC, 0660);
        if (fd_ < 0 || ::ftruncate(fd_, kRingBytes) != 0) { *error = "shared-memory ring: " + std::string(std::strerror(errno)); return false; }
        map_ = static_cast<uint8_t *>(::mmap(nullptr, kRingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (map_ == MAP_FAILED) { map_ = nullptr; *error = "shared-memory mmap: " + std::string(std::strerror(errno)); return false; }
        std::memset(map_, 0, kRingBytes); header_ = reinterpret_cast<tb330fu_video_ring_header *>(map_);
        std::memcpy(header_->magic, TB330FU_VIDEO_RING_MAGIC, 8); header_->version = TB330FU_VIDEO_RING_VERSION;
        header_->header_bytes = sizeof(*header_); header_->slot_count = TB330FU_VIDEO_RING_SLOTS;
        header_->slot_bytes = TB330FU_VIDEO_RING_SLOT_BYTES; header_->slots_offset = sizeof(*header_);
        header_->timestamp_domain = TB330FU_VIDEO_TIMESTAMP_MONOTONIC_NS; return true;
    }
    bool prepare_muxer(std::string *error) {
        int rc = avformat_alloc_output_context2(&format_, nullptr, "mp4", TB330FU_VIDEO_OUTPUT_PART);
        if (rc < 0 || !format_) { *error = "MP4 context: " + av_error(rc); return false; }
        const AVCodec *codec = avcodec_find_encoder_by_name("h264_v4l2m2m");
        if (!codec) { *error = "h264_v4l2m2m not found"; return false; }
        stream_ = avformat_new_stream(format_, nullptr); codec_ = avcodec_alloc_context3(codec);
        if (!stream_ || !codec_) { *error = "FFmpeg allocation failed"; return false; }
        codec_->width = 1920; codec_->height = 1080; codec_->pix_fmt = AV_PIX_FMT_NV21;
        codec_->time_base = AVRational{1, 30}; codec_->framerate = AVRational{30, 1}; codec_->bit_rate = 4000000;
        codec_->gop_size = 30; codec_->max_b_frames = 0;
        av_opt_set_int(codec_->priv_data, "num_output_buffers", 12, 0);
        av_opt_set_int(codec_->priv_data, "num_capture_buffers", 12, 0);
        rc = avcodec_open2(codec_, codec, nullptr); if (rc < 0) { *error = "encoder open: " + av_error(rc); return false; }
        rc = avcodec_parameters_from_context(stream_->codecpar, codec_); if (rc < 0) { *error = "codec parameters: " + av_error(rc); return false; }
        const bool valid_avcc = stream_->codecpar->extradata &&
            stream_->codecpar->extradata_size >= 7 &&
            stream_->codecpar->extradata[0] == 1 &&
            stream_->codecpar->extradata[1] != 0;
        if (!valid_avcc) {
            auto *extra = static_cast<uint8_t *>(av_mallocz(sizeof(kMtkH264Avcc) + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!extra) { *error = "codec extradata allocation failed"; return false; }
            std::memcpy(extra, kMtkH264Avcc, sizeof(kMtkH264Avcc));
            stream_->codecpar->extradata = extra;
            stream_->codecpar->extradata_size = sizeof(kMtkH264Avcc);
        }
        last_packet_pts_ = AV_NOPTS_VALUE;
        last_packet_dts_ = AV_NOPTS_VALUE;
        stream_->time_base = codec_->time_base;
        if (!(format_->oformat->flags & AVFMT_NOFILE)) { rc = avio_open(&format_->pb, TB330FU_VIDEO_OUTPUT_PART, AVIO_FLAG_WRITE); if (rc < 0) { *error = "output open: " + av_error(rc); return false; } }
        rc = avformat_write_header(format_, nullptr); if (rc < 0) { *error = "MP4 header: " + av_error(rc); return false; }
        return true;
    }
    void cleanup_ffmpeg_locked() {
        if (codec_) avcodec_free_context(&codec_);
        if (format_ && format_->pb) avio_closep(&format_->pb);
        if (format_) avformat_free_context(format_);
        format_ = nullptr;
        stream_ = nullptr;
    }
    tb330fu_video_slot *slot(uint64_t seq) const { auto *p = map_ + header_->slots_offset + (seq % header_->slot_count) * tb330fu_video_slot_bytes(header_->slot_bytes); return reinterpret_cast<tb330fu_video_slot *>(p); }
    void consume() {
        uint64_t first = 0, last = 0;
        for (;;) {
            if (!active_) break;
            uint64_t seq = __atomic_load_n(&header_->read_sequence, __ATOMIC_RELAXED); auto *s = slot(seq);
            uint64_t write = __atomic_load_n(&header_->write_sequence, __ATOMIC_ACQUIRE);
            if (seq >= write) { if (!active_) break; std::this_thread::yield(); continue; }
            if (__atomic_load_n(&s->sequence, __ATOMIC_ACQUIRE) != seq) { std::this_thread::yield(); continue; }
            if (s->layout != TB330FU_VIDEO_LAYOUT_MAPPER_Y_VU || s->plane_count != 2 || s->width != 1920 || s->height != 1080) { std::lock_guard<std::mutex> l(mu_); status_.set_state(RECORDING_ERROR); status_.set_last_error("unexpected mapper source layout"); active_ = false; break; }
            if (last && s->capture_timestamp_ns <= last) { __atomic_store_n(&header_->read_sequence, seq + 1, __ATOMIC_RELEASE); continue; }
            if (!first) first = s->capture_timestamp_ns; last = s->capture_timestamp_ns;
            AVFrame *f = av_frame_alloc(); if (!f) break; f->format = AV_PIX_FMT_NV21; f->width = 1920; f->height = 1080;
            f->pts = av_rescale_q(static_cast<int64_t>(s->capture_timestamp_ns - first), AVRational{1, 1000000000}, codec_->time_base);
            f->buf[0] = buffer(1920u * 1080u); f->buf[1] = buffer(1920u * 540u);
            if (!f->buf[0] || !f->buf[1]) { av_frame_free(&f); break; }
            f->data[0] = f->buf[0]->data; f->data[1] = f->buf[1]->data; f->linesize[0] = 1920; f->linesize[1] = 1920;
            for (uint32_t y = 0; y < 1080; ++y) std::memcpy(f->data[0] + y * 1920, s->payload + s->planes[0].offset + y * s->planes[0].row_stride, 1920);
            for (uint32_t y = 0; y < 540; ++y) std::memcpy(f->data[1] + y * 1920, s->payload + s->planes[1].offset + y * s->planes[1].row_stride, 1920);
            int rc = avcodec_send_frame(codec_, f); av_frame_free(&f); if (rc >= 0) rc = receive_packets();
            if (rc < 0) { std::lock_guard<std::mutex> l(mu_); status_.set_state(RECORDING_ERROR); status_.set_last_error("encode: " + av_error(rc)); active_ = false; break; }
            __atomic_store_n(&header_->read_sequence, seq + 1, __ATOMIC_RELEASE);
            std::lock_guard<std::mutex> l(mu_); status_.set_frames_received(status_.frames_received() + 1); status_.set_frames_encoded(status_.frames_encoded() + 1); status_.set_last_video_timestamp_ns(last);
        }
    }
    int receive_packets() {
        AVPacket *p = av_packet_alloc(); if (!p) return AVERROR(ENOMEM); int rc = 0;
        while ((rc = avcodec_receive_packet(codec_, p)) >= 0) {
            if (is_h264_parameter_set_packet(p)) {
                av_packet_unref(p);
                continue;
            }
            p->stream_index = stream_->index;
            av_packet_rescale_ts(p, codec_->time_base, stream_->time_base);
            if (p->dts != AV_NOPTS_VALUE && last_packet_dts_ != AV_NOPTS_VALUE && p->dts <= last_packet_dts_)
                p->dts = last_packet_dts_ + 1;
            if (p->pts != AV_NOPTS_VALUE && last_packet_pts_ != AV_NOPTS_VALUE && p->pts <= last_packet_pts_)
                p->pts = last_packet_pts_ + 1;
            if (p->pts != AV_NOPTS_VALUE && p->dts != AV_NOPTS_VALUE && p->pts < p->dts)
                p->pts = p->dts;
            last_packet_pts_ = p->pts;
            last_packet_dts_ = p->dts;
            int w = av_interleaved_write_frame(format_, p); av_packet_unref(p); if (w < 0) { rc = w; break; }
        }
        av_packet_free(&p); return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : rc;
    }
    uint64_t part_bytes() const { struct stat st{}; if (::stat(TB330FU_VIDEO_OUTPUT_PART, &st) == 0) return st.st_size; return ::stat(TB330FU_VIDEO_OUTPUT, &st) == 0 ? st.st_size : 0; }
    void cleanup_locked() { cleanup_ffmpeg_locked(); if (map_) ::munmap(map_, kRingBytes); map_ = nullptr; if (fd_ >= 0) ::close(fd_); fd_ = -1; header_ = nullptr; }
    std::mutex mu_; VideoStatus status_; std::atomic<bool> active_{false}; std::thread worker_; int fd_ = -1; uint8_t *map_ = nullptr; tb330fu_video_ring_header *header_ = nullptr;
    AVFormatContext *format_ = nullptr; AVCodecContext *codec_ = nullptr; AVStream *stream_ = nullptr;
    int64_t last_packet_pts_ = AV_NOPTS_VALUE; int64_t last_packet_dts_ = AV_NOPTS_VALUE;
};

void send_response(int fd, const Response &r) { std::string wire; r.SerializeToString(&wire); ::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL); }
void client(int fd, Recorder &rec) {
    uint8_t buf[65536]; ssize_t n = ::recv(fd, buf, sizeof(buf), 0); Request q; Response r;
    if (n <= 0 || !q.ParseFromArray(buf, static_cast<int>(n))) { r.mutable_error()->set_code(INVALID_REQUEST); r.mutable_error()->set_message("malformed recorder request"); send_response(fd, r); ::close(fd); return; }
    r.set_request_id(q.request_id()); std::string error;
    if (q.has_video_start()) { if (!rec.start(&error)) { r.mutable_error()->set_code(BUSY); r.mutable_error()->set_message(error); } else r.mutable_video()->CopyFrom(rec.status()); }
    else if (q.has_video_stop()) { if (!rec.stop(&error)) { r.mutable_error()->set_code(INTERNAL_ERROR); r.mutable_error()->set_message(error); } else r.mutable_video()->CopyFrom(rec.status()); }
    else if (q.has_video_status()) r.mutable_video()->CopyFrom(rec.status());
    else { r.mutable_error()->set_code(INVALID_REQUEST); r.mutable_error()->set_message("recorder supports video operations only"); }
    send_response(fd, r); ::close(fd);
}
}

int main() {
    ::mkdir("/run/tb330fu-camera", 0775); ::mkdir("/var/lib/tb330fu-camera", 0775); ::unlink(TB330FU_VIDEO_RECORDER_SOCKET);
    int s = ::socket(AF_UNIX, SOCK_SEQPACKET, 0); if (s < 0) return 2; sockaddr_un a{}; a.sun_family = AF_UNIX; std::strncpy(a.sun_path, TB330FU_VIDEO_RECORDER_SOCKET, sizeof(a.sun_path)-1);
    if (::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0 || ::listen(s, 8) < 0) return 3; ::chmod(TB330FU_VIDEO_RECORDER_SOCKET, 0666);
    Recorder recorder; for (;;) { int c = ::accept(s, nullptr, nullptr); if (c >= 0) std::thread(client, c, std::ref(recorder)).detach(); }
}
