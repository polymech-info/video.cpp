#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "pm/video/capture.hpp"

#include <linux/videodev2.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <jpeglib.h>
#include <setjmp.h>
#include "string/string_utils.hpp"

namespace fs = std::filesystem;
namespace pm::video {

namespace {

static bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

static int xioctl(int fd, unsigned long request, void* arg) {
    for (;;) {
        int r = ioctl(fd, request, arg);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

// V4L2 MJPEG: drivers often leave a large zero-padded prefix before SOI (FF D8).
// Strip only that prefix — do not search for FF D9: 0xFF 0xD9 can appear inside entropy-coded
// scan data (rare but valid), which would truncate the image to a top band.
// Keep bytes [SOI .. bytesused); trailing zero padding after the real EOI is harmless for decoders.
// Returns false if no SOI was found (skip frame, wait for next buffer).
static bool copy_mjpeg_trimmed(const uint8_t* p, size_t len, std::vector<uint8_t>& dst) {
    if (!p || len < 2) {
        dst.clear();
        return false;
    }
    size_t start = 0;
    for (; start + 2 <= len; ++start) {
        if (p[start] == 0xff && p[start + 1] == 0xd8)
            break;
    }
    if (start + 2 > len) {
        dst.clear();
        return false;
    }
    dst.assign(p + start, p + len);
    return !dst.empty();
}

static std::string fourcc_label(uint32_t f) {
    char s[5];
    s[0] = static_cast<char>(f & 0xFF);
    s[1] = static_cast<char>((f >> 8) & 0xFF);
    s[2] = static_cast<char>((f >> 16) & 0xFF);
    s[3] = static_cast<char>((f >> 24) & 0xFF);
    s[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        if (s[i] < 0x20 || s[i] > 0x7E) s[i] = '?';
    }
    if (f == V4L2_PIX_FMT_MJPEG) return "MJPEG";
    if (f == V4L2_PIX_FMT_YUYV) return "YUY2";
    if (f == V4L2_PIX_FMT_NV12) return "NV12";
    return std::string(s);
}

static void yuyv_to_bgr24(const uint8_t* src, int width, int height, std::vector<uint8_t>& dst) {
    dst.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
    auto clamp = [](int v) -> uint8_t {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return static_cast<uint8_t>(v);
    };
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(width) * 2u;
        uint8_t*       out = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        for (int x = 0; x < width; x += 2) {
            const uint8_t* p  = row + static_cast<size_t>(x) * 2u;
            int             y0 = static_cast<int>(p[0]) - 16;
            int             u  = static_cast<int>(p[1]) - 128;
            int             y1 = static_cast<int>(p[2]) - 16;
            int             v  = static_cast<int>(p[3]) - 128;
            int             r0 = 298 * y0 + 409 * v + 128;
            int             g0 = 298 * y0 - 100 * u - 208 * v + 128;
            int             b0 = 298 * y0 + 516 * u + 128;
            int             r1 = 298 * y1 + 409 * v + 128;
            int             g1 = 298 * y1 - 100 * u - 208 * v + 128;
            int             b1 = 298 * y1 + 516 * u + 128;
            out[x * 3 + 0]     = clamp(b0 >> 8);
            out[x * 3 + 1]     = clamp(g0 >> 8);
            out[x * 3 + 2]     = clamp(r0 >> 8);
            out[(x + 1) * 3 + 0] = clamp(b1 >> 8);
            out[(x + 1) * 3 + 1] = clamp(g1 >> 8);
            out[(x + 1) * 3 + 2] = clamp(r1 >> 8);
        }
    }
}

static std::vector<std::string> list_video_nodes() {
    std::vector<std::string> out;
    std::error_code            ec;
    if (!fs::exists("/dev", ec)) return out;
    for (const auto& e : fs::directory_iterator("/dev", ec)) {
        if (ec) break;
        std::string name = e.path().filename().string();
        if (name.size() >= 5 && name.compare(0, 5, "video") == 0)
            out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static bool query_card(int fd, std::string& card_out) {
    struct v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) return false;
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) return false;
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) return false;
    card_out.assign(reinterpret_cast<const char*>(cap.card));
    const auto n = card_out.find('\0');
    if (n != std::string::npos) card_out.resize(n);
    return true;
}

// Returns fd on success (caller closes). device_name: substring match on card or full path.
static int open_capture_device(const std::string& device_name, std::string& opened_card) {
    const auto nodes = list_video_nodes();
    if (nodes.empty()) return -1;

    for (const auto& path : nodes) {
        if (!device_name.empty() && icontains(path, device_name)) {
            int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK, 0);
            if (fd < 0) continue;
            std::string card;
            if (query_card(fd, card)) {
                opened_card = card;
                return fd;
            }
            ::close(fd);
        }
    }

    for (const auto& path : nodes) {
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd < 0) continue;
        std::string card;
        if (!query_card(fd, card)) {
            ::close(fd);
            continue;
        }
        if (device_name.empty() || icontains(card, device_name)) {
            opened_card = card;
            return fd;
        }
        ::close(fd);
    }
    return -1;
}

static bool negotiate_format(int fd, uint32_t pixfmt, int req_w, int req_h,
                             int& out_w, int& out_h, uint32_t& out_fmt) {
    struct v4l2_format fmt{};
    fmt.type                 = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width        = static_cast<uint32_t>(req_w > 0 ? req_w : 1280);
    fmt.fmt.pix.height       = static_cast<uint32_t>(req_h > 0 ? req_h : 720);
    fmt.fmt.pix.pixelformat  = pixfmt;
    fmt.fmt.pix.field        = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) return false;
    out_w  = static_cast<int>(fmt.fmt.pix.width);
    out_h  = static_cast<int>(fmt.fmt.pix.height);
    out_fmt = fmt.fmt.pix.pixelformat;
    return out_w > 0 && out_h > 0;
}

static bool pick_format_and_size(int fd, int req_w, int req_h,
                                 uint32_t& pixfmt_out, int& w_out, int& h_out) {
    static const uint32_t k_formats[] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV };
    const int             try_w[]     = { req_w, 1920, 1280, 640, 352 };
    const int             try_h[]     = { req_h, 1080, 720,  480, 288 };

    for (uint32_t pix : k_formats) {
        for (size_t i = 0; i < sizeof(try_w) / sizeof(try_w[0]); ++i) {
            int w = try_w[i] > 0 ? try_w[i] : 1280;
            int h = try_h[i] > 0 ? try_h[i] : 720;
            int ow = 0, oh = 0;
            uint32_t got = 0;
            if (negotiate_format(fd, pix, w, h, ow, oh, got) && got == pix) {
                pixfmt_out = pix;
                w_out      = ow;
                h_out      = oh;
                return true;
            }
        }
    }
    return false;
}

struct MmapBuf {
    void*  start = nullptr;
    size_t len   = 0;
};

} // namespace

// ── encode_jpeg / save_frame ─────────────────────────────────────────────────

std::vector<uint8_t> encode_jpeg(const VideoFrame& frame, int quality) {
    if (frame.data.empty() || frame.width <= 0 || frame.height <= 0) return {};
    if (frame.format != PixelFormat::BGR24) return {};

    int w = frame.width;
    int h = frame.height;
    int stride = frame.stride > 0 ? frame.stride : w * 3;

    struct JpegErrorMgr {
        struct jpeg_error_mgr pub;
        jmp_buf                 setjmp_buffer;
    } jerr;

    jpeg_compress_struct cinfo{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = [](j_common_ptr c) {
        auto* je = reinterpret_cast<JpegErrorMgr*>(c->err);
        longjmp(je->setjmp_buffer, 1);
    };

    std::vector<uint8_t> out;
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_compress(&cinfo);
        return {};
    }

    jpeg_create_compress(&cinfo);
    unsigned char*       mem   = nullptr;
    unsigned long        size  = 0;
    jpeg_mem_dest(&cinfo, &mem, &size);

    cinfo.image_width      = static_cast<JDIMENSION>(w);
    cinfo.image_height     = static_cast<JDIMENSION>(h);
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    std::vector<uint8_t> row(static_cast<size_t>(w) * 3u);
    while (cinfo.next_scanline < static_cast<JDIMENSION>(h)) {
        const uint8_t* src =
            frame.data.data() + static_cast<size_t>(cinfo.next_scanline) * static_cast<size_t>(stride);
        for (int x = 0; x < w; ++x) {
            row[static_cast<size_t>(x) * 3u + 0] = src[static_cast<size_t>(x) * 3u + 2]; // R
            row[static_cast<size_t>(x) * 3u + 1] = src[static_cast<size_t>(x) * 3u + 1]; // G
            row[static_cast<size_t>(x) * 3u + 2] = src[static_cast<size_t>(x) * 3u + 0]; // B
        }
        JSAMPROW rowptr = row.data();
        jpeg_write_scanlines(&cinfo, &rowptr, 1);
    }

    jpeg_finish_compress(&cinfo);
    if (mem && size > 0) {
        out.assign(mem, mem + size);
        free(mem);
    }
    jpeg_destroy_compress(&cinfo);
    return out;
}

bool save_frame(const VideoFrame& frame, const std::string& path, std::string& err, int jpeg_quality) {
    std::string lower = path;
    lower = media::strutil::lower_ascii(std::move(lower));
    const bool is_jpg = lower.size() >= 4 &&
                        (lower.compare(lower.size() - 4, 4, ".jpg") == 0 ||
                         (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".jpeg") == 0));
    if (!is_jpg) {
        err = "video (linux): only .jpg / .jpeg are supported";
        return false;
    }
    if (frame.data.empty() || frame.width <= 0 || frame.height <= 0) {
        err = "empty frame";
        return false;
    }

    if (frame.format == PixelFormat::MJPEG) {
        std::vector<uint8_t> trimmed;
        if (!copy_mjpeg_trimmed(frame.data.data(), frame.data.size(), trimmed)) {
            err = "mjpeg: no JPEG SOI in frame";
            return false;
        }
        std::ofstream f(path, std::ios::binary);
        if (!f) {
            err = "cannot open file";
            return false;
        }
        f.write(reinterpret_cast<const char*>(trimmed.data()),
                static_cast<std::streamsize>(trimmed.size()));
        return f.good();
    }

    if (frame.format == PixelFormat::BGR24) {
        auto jpg = encode_jpeg(frame, jpeg_quality);
        if (jpg.empty()) {
            err = "jpeg encode failed";
            return false;
        }
        std::ofstream f(path, std::ios::binary);
        if (!f) {
            err = "cannot open file";
            return false;
        }
        f.write(reinterpret_cast<const char*>(jpg.data()),
                static_cast<std::streamsize>(jpg.size()));
        return f.good();
    }

    err = "unsupported pixel format for save";
    return false;
}

bool load_frame(const std::string& /*path*/, VideoFrame& /*out*/, std::string& err) {
    err = "load_frame: not implemented on linux yet";
    return false;
}

// ── enumerate_capture_devices ───────────────────────────────────────────────

std::vector<DeviceInfo> enumerate_capture_devices() {
    std::vector<DeviceInfo> out;
    for (const auto& path : list_video_nodes()) {
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd < 0) continue;
        std::string card;
        if (!query_card(fd, card)) {
            ::close(fd);
            continue;
        }
        ::close(fd);
        DeviceInfo d;
        d.id          = path;
        d.name        = std::move(card);
        d.is_default  = out.empty();
        out.push_back(std::move(d));
    }
    return out;
}

// ── enumerate_device_modes ─────────────────────────────────────────────────

std::vector<DeviceMode> enumerate_device_modes(const std::string& device_name) {
    std::string opened;
    int         fd = open_capture_device(device_name, opened);
    if (fd < 0) return {};
    struct Closer {
        int f;
        ~Closer() {
            if (f >= 0) ::close(f);
        }
    } closer{ fd };

    std::vector<DeviceMode> modes;
    struct v4l2_fmtdesc fmtdesc{};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fmtdesc.index = 0;; ++fmtdesc.index) {
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0) break;
        uint32_t pix = fmtdesc.pixelformat;

        struct v4l2_frmsizeenum frmsize{};
        frmsize.pixel_format = pix;
        for (frmsize.index = 0;; ++frmsize.index) {
            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0) break;
            int w = 0, h = 0;
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                w = static_cast<int>(frmsize.discrete.width);
                h = static_cast<int>(frmsize.discrete.height);
            } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                w = static_cast<int>(frmsize.stepwise.max_width);
                h = static_cast<int>(frmsize.stepwise.max_height);
            } else
                continue;

            struct v4l2_frmivalenum iv{};
            iv.pixel_format = pix;
            iv.width        = static_cast<uint32_t>(w);
            iv.height       = static_cast<uint32_t>(h);
            bool got_iv = false;
            for (iv.index = 0;; ++iv.index) {
                if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) < 0) break;
                DeviceMode m;
                m.width  = w;
                m.height = h;
                m.format = fourcc_label(pix);
                if (iv.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                    m.fps_num = static_cast<int>(iv.discrete.numerator);
                    m.fps_den = static_cast<int>(iv.discrete.denominator);
                } else if (iv.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
                    m.fps_num = static_cast<int>(iv.stepwise.max.numerator);
                    m.fps_den = static_cast<int>(iv.stepwise.max.denominator);
                } else
                    continue;
                if (m.fps_den <= 0) m.fps_den = 1;
                modes.push_back(m);
                got_iv = true;
            }
            if (!got_iv) {
                DeviceMode m;
                m.width  = w;
                m.height = h;
                m.format = fourcc_label(pix);
                m.fps_num = 30;
                m.fps_den = 1;
                modes.push_back(m);
            }
        }
    }
    return modes;
}

// ── VideoInput ──────────────────────────────────────────────────────────────

struct VideoInput::Impl {
    int                   fd       = -1;
    std::vector<MmapBuf>  buffers;
    std::atomic<bool>     running{ false };
    std::thread           capture_thread;
    std::function<void(const VideoFrame&)> on_frame;
    std::string           device_label;
    std::atomic<int>      actual_w{ 0 };
    std::atomic<int>      actual_h{ 0 };
    uint32_t              pixfmt = 0;

    void stream_off() {
        if (fd < 0) return;
        enum v4l2_buf_type typ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd, VIDIOC_STREAMOFF, &typ);
    }

    void unmap_all() {
        for (auto& b : buffers) {
            if (b.start && b.len) munmap(b.start, b.len);
            b = {};
        }
        buffers.clear();
    }

    void close_fd() {
        unmap_all();
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    void read_loop() {
        std::vector<uint8_t> bgr_scratch;
        while (running.load()) {
            struct pollfd pfd {};
            pfd.fd     = fd;
            pfd.events = POLLIN;
            int pr     = poll(&pfd, 1, 500);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) continue;

            struct v4l2_buffer buf {};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) continue;
                break;
            }

            if (buf.index < buffers.size() && buffers[buf.index].start && buf.bytesused > 0) {
                const uint8_t* p = static_cast<const uint8_t*>(buffers[buf.index].start);
                auto           ts =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

                VideoFrame vf;
                vf.timestamp_ms = ts;
                vf.width        = actual_w.load();
                vf.height       = actual_h.load();

                if (pixfmt == V4L2_PIX_FMT_MJPEG) {
                    vf.format = PixelFormat::MJPEG;
                    vf.stride = 0;
                    if (!copy_mjpeg_trimmed(p, buf.bytesused, vf.data)) {
                        xioctl(fd, VIDIOC_QBUF, &buf);
                        continue;
                    }
                } else if (pixfmt == V4L2_PIX_FMT_YUYV) {
                    yuyv_to_bgr24(p, vf.width, vf.height, bgr_scratch);
                    vf.format = PixelFormat::BGR24;
                    vf.stride = vf.width * 3;
                    vf.data   = bgr_scratch;
                }

                if (on_frame && !vf.data.empty()) on_frame(vf);
            }

            xioctl(fd, VIDIOC_QBUF, &buf);
        }
        stream_off();
    }
};

VideoInput::VideoInput() : m_(std::make_unique<Impl>()) {}
VideoInput::~VideoInput() { stop(); }

void VideoInput::start(const std::string& device_name, int width, int height, int /*fps*/,
                       std::function<void(const VideoFrame&)> on_frame_cb) {
    stop();

    std::string card;
    int         fd = open_capture_device(device_name, card);
    if (fd < 0)
        throw std::runtime_error("video: no V4L2 capture device found"
                                 + (device_name.empty() ? "" : (": " + device_name)));

    uint32_t pix = 0;
    int      w = 0, h = 0;
    if (!pick_format_and_size(fd, width, height, pix, w, h)) {
        ::close(fd);
        throw std::runtime_error("video: could not negotiate pixel format / resolution");
    }

    struct v4l2_requestbuffers req {};
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        ::close(fd);
        throw std::runtime_error("video: VIDIOC_REQBUFS failed");
    }

    m_->buffers.resize(req.count);
    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_buffer b {};
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            m_->unmap_all();
            ::close(fd);
            throw std::runtime_error("video: VIDIOC_QUERYBUF failed");
        }
        void* a =
            mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (a == MAP_FAILED) {
            m_->unmap_all();
            ::close(fd);
            throw std::runtime_error("video: mmap failed");
        }
        m_->buffers[i].start = a;
        m_->buffers[i].len   = b.length;
    }

    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_buffer b {};
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = i;
        xioctl(fd, VIDIOC_QBUF, &b);
    }

    enum v4l2_buf_type typ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &typ) < 0) {
        m_->unmap_all();
        ::close(fd);
        throw std::runtime_error("video: STREAMON failed");
    }

    m_->fd          = fd;
    m_->pixfmt      = pix;
    m_->device_label = std::move(card);
    m_->actual_w.store(w);
    m_->actual_h.store(h);
    m_->on_frame    = std::move(on_frame_cb);
    m_->running.store(true);
    m_->capture_thread = std::thread([this] { m_->read_loop(); });
}

void VideoInput::stop() {
    if (!m_->running.exchange(false)) return;
    if (m_->capture_thread.joinable()) m_->capture_thread.join();
    m_->close_fd();
    m_->on_frame = {};
}

bool VideoInput::is_running() const { return m_->running.load(); }

const std::string& VideoInput::opened_device_name() const { return m_->device_label; }

int VideoInput::actual_width() const { return m_->actual_w.load(); }

int VideoInput::actual_height() const { return m_->actual_h.load(); }

// ── capture_still ─────────────────────────────────────────────────────────────

VideoFrame capture_still(const std::string& device_name,
                         int width,
                         int height,
                         int timeout_ms,
                         std::function<bool()> cancel_fn) {
    VideoFrame         result;
    std::mutex         mu;
    std::condition_variable cv;
    bool               got = false;

    VideoInput vin;
    vin.start(device_name, width, height, 0,
              [&](const VideoFrame& f) {
                  std::lock_guard<std::mutex> lk(mu);
                  if (!got) {
                      result = f;
                      got    = true;
                      cv.notify_one();
                  }
              });

    {
        std::unique_lock<std::mutex> lk(mu);
        const auto timeout = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 5000);
        const auto start   = std::chrono::steady_clock::now();
        while (!got) {
            if (cancel_fn && cancel_fn()) {
                vin.stop();
                throw std::runtime_error("video: capture cancelled");
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= timeout) {
                vin.stop();
                throw std::runtime_error("video: timeout waiting for first frame");
            }
            const auto slice = std::min(std::chrono::milliseconds(80),
                                        std::chrono::duration_cast<std::chrono::milliseconds>(timeout - elapsed));
            cv.wait_for(lk, slice, [&] { return got; });
        }
    }
    vin.stop();
    return result;
}

} // namespace pm::video

#endif // FEATURE_VIDEO
