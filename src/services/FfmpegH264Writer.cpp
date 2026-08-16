#include "lpr/services/FfmpegH264Writer.h"
#include "lpr/Log.h"

#ifdef LPR_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#endif

namespace lpr {

#ifdef LPR_WITH_FFMPEG

struct FfmpegH264Writer::Impl {
    AVFormatContext* fmt   = nullptr;
    AVCodecContext*  enc   = nullptr;
    AVStream*        st    = nullptr;
    AVFrame*         frame = nullptr;
    AVPacket*        pkt   = nullptr;
    SwsContext*      sws   = nullptr;
    int64_t          pts   = 0;
    int              w = 0, h = 0;
};

FfmpegH264Writer::~FfmpegH264Writer() { close(); }

bool FfmpegH264Writer::open(const std::string& path, double fps, cv::Size size, int crf) {
    close();
    if (size.width <= 0 || size.height <= 0 || fps <= 0.0) return false;

    const AVCodec* codec = avcodec_find_encoder_by_name("h264_mf");
    if (!codec) { LOGW() << "FfmpegH264Writer: h264_mf encoder not available in this FFmpeg"; return false; }

    auto* d = new Impl();
    impl_ = d;
    d->w = size.width;
    d->h = size.height;
    const int fpsI = std::max(1, (int)std::lround(fps));

    if (avformat_alloc_output_context2(&d->fmt, nullptr, "mp4", path.c_str()) < 0 || !d->fmt) {
        LOGW() << "FfmpegH264Writer: alloc output context failed"; close(); return false;
    }
    d->st = avformat_new_stream(d->fmt, nullptr);
    if (!d->st) { close(); return false; }

    d->enc = avcodec_alloc_context3(codec);
    if (!d->enc) { close(); return false; }
    d->enc->width        = d->w;
    d->enc->height       = d->h;
    d->enc->pix_fmt      = AV_PIX_FMT_NV12;      // Media Foundation's H.264 input format
    d->enc->time_base    = AVRational{1, fpsI};
    d->enc->framerate    = AVRational{fpsI, 1};
    d->enc->gop_size     = fpsI * 2;             // a keyframe every ~2s (seekable, still small)
    d->enc->max_b_frames = 0;                    // MF + broad playback compatibility

    // MF has no CRF; derive a modest bitrate from resolution×fps (H.264 is efficient, so this
    // stays well under the old mp4v output). The CRF knob still nudges it: higher CRF -> lower bpp.
    double bpp = 0.16 - 0.004 * static_cast<double>(crf - 18);
    bpp = std::clamp(bpp, 0.03, 0.16);
    long long br = static_cast<long long>(static_cast<double>(d->w) * d->h * fpsI * bpp);
    d->enc->bit_rate = std::max<long long>(br, 300000);

    if (d->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        d->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "hw_encoding", "0", 0);   // force the SOFTWARE MFT so it accepts CPU frames
    const int rc = avcodec_open2(d->enc, codec, &opts);
    av_dict_free(&opts);
    if (rc < 0) { LOGW() << "FfmpegH264Writer: avcodec_open2(h264_mf) failed (" << rc << ")"; close(); return false; }

    if (avcodec_parameters_from_context(d->st->codecpar, d->enc) < 0) { close(); return false; }
    d->st->time_base = d->enc->time_base;

    if (!(d->fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&d->fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            LOGW() << "FfmpegH264Writer: avio_open failed for " << path; close(); return false;
        }
    }
    if (avformat_write_header(d->fmt, nullptr) < 0) { LOGW() << "FfmpegH264Writer: write_header failed"; close(); return false; }

    d->frame = av_frame_alloc();
    if (!d->frame) { close(); return false; }
    d->frame->format = AV_PIX_FMT_NV12;
    d->frame->width  = d->w;
    d->frame->height = d->h;
    if (av_frame_get_buffer(d->frame, 32) < 0) { close(); return false; }

    d->pkt = av_packet_alloc();
    d->sws = sws_getContext(d->w, d->h, AV_PIX_FMT_BGR24,
                            d->w, d->h, AV_PIX_FMT_NV12,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!d->pkt || !d->sws) { close(); return false; }

    opened_ = true;
    return true;
}

bool FfmpegH264Writer::write(const cv::Mat& bgr) {
    if (!opened_ || !impl_) return false;
    auto* d = impl_;
    if (bgr.empty()) return true;

    cv::Mat in = bgr;
    if (in.cols != d->w || in.rows != d->h) cv::resize(in, in, cv::Size(d->w, d->h));
    if (in.type() != CV_8UC3) return false;   // expect a BGR frame

    if (av_frame_make_writable(d->frame) < 0) return false;
    const uint8_t* srcData[1] = { in.data };
    const int      srcStride[1] = { static_cast<int>(in.step) };
    sws_scale(d->sws, srcData, srcStride, 0, d->h, d->frame->data, d->frame->linesize);
    d->frame->pts = d->pts++;

    if (avcodec_send_frame(d->enc, d->frame) < 0) return false;
    for (;;) {
        const int rc = avcodec_receive_packet(d->enc, d->pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) return false;
        av_packet_rescale_ts(d->pkt, d->enc->time_base, d->st->time_base);
        d->pkt->stream_index = d->st->index;
        av_interleaved_write_frame(d->fmt, d->pkt);
        av_packet_unref(d->pkt);
    }
    return true;
}

void FfmpegH264Writer::close() {
    opened_ = false;
    auto* d = impl_;
    if (!d) return;

    // Flush the encoder (drain buffered packets), then finalize the container.
    if (d->enc && d->fmt && d->pkt) {
        if (avcodec_send_frame(d->enc, nullptr) == 0) {
            while (avcodec_receive_packet(d->enc, d->pkt) == 0) {
                av_packet_rescale_ts(d->pkt, d->enc->time_base, d->st->time_base);
                d->pkt->stream_index = d->st->index;
                av_interleaved_write_frame(d->fmt, d->pkt);
                av_packet_unref(d->pkt);
            }
        }
    }
    if (d->fmt && d->fmt->pb) av_write_trailer(d->fmt);

    if (d->sws)   sws_freeContext(d->sws);
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt)   av_packet_free(&d->pkt);
    if (d->enc)   avcodec_free_context(&d->enc);
    if (d->fmt) {
        if (d->fmt->pb && !(d->fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&d->fmt->pb);
        avformat_free_context(d->fmt);
    }
    delete d;
    impl_ = nullptr;
}

#else  // ---- FFmpeg dev libs not available: stub so the module still builds/links ----

FfmpegH264Writer::~FfmpegH264Writer() {}
bool FfmpegH264Writer::open(const std::string&, double, cv::Size, int) { return false; }
bool FfmpegH264Writer::write(const cv::Mat&) { return false; }
void FfmpegH264Writer::close() { opened_ = false; }

#endif

} // namespace lpr
