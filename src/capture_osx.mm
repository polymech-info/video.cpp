#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

// ── macOS AVFoundation video capture backend ───────────────────────────────
// Implements pm::video::{enumerate_capture_devices, enumerate_device_modes,
//   VideoInput, capture_still, encode_jpeg, save_frame}
// Frameworks: AVFoundation CoreMedia CoreVideo ImageIO Foundation

#import <AVFoundation/AVFoundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
// Use UTI string directly to avoid the deprecated kUTTypeJPEG constant (macOS < 12).
#ifndef PM_UTI_JPEG
#  define PM_UTI_JPEG CFSTR("public.jpeg")
#endif

#include "pm/video/capture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ── utilities ─────────────────────────────────────────────────────────────────

static std::string ns_to_std(NSString* s) {
    if (!s) return {};
    return {[s UTF8String]};
}

static NSString* std_to_ns(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()];
}

static bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    return it != haystack.end();
}

static NSArray<AVCaptureDevice*>* av_video_devices() {
    // AVCaptureDeviceTypeExternal requires macOS 14+; fall back to the
    // deprecated ExternalUnknown on older SDKs so we compile everywhere.
    NSArray* types;
#if defined(MAC_OS_X_VERSION_14_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_14_0
    types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera,
              AVCaptureDeviceTypeExternal];
#else
    types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera,
              AVCaptureDeviceTypeExternalUnknown];
#endif
    AVCaptureDeviceDiscoverySession* s =
        [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:types
            mediaType:AVMediaTypeVideo
            position:AVCaptureDevicePositionUnspecified];
    return s.devices;
}

// Empty needle → default camera. Else exact uniqueID, then friendly-name substring.
static AVCaptureDevice* find_av_device(NSArray<AVCaptureDevice*>* devs, const std::string& needle) {
    if (needle.empty())
        return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    AVCaptureDevice* name_hit = nil;
    for (AVCaptureDevice* d in devs) {
        if (ns_to_std(d.uniqueID) == needle)
            return d;
        if (!name_hit && icontains(ns_to_std(d.localizedName), needle))
            name_hit = d;
    }
    return name_hit;
}

static std::string fourcc_name(FourCharCode fcc) {
    switch (fcc) {
        case kCVPixelFormatType_422YpCbCr8:                      return "UYVY";
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:    return "NV12";
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:     return "NV12";
        case kCVPixelFormatType_32BGRA:                          return "BGRA";
        case kCVPixelFormatType_24BGR:                           return "BGR24";
        case kCVPixelFormatType_24RGB:                           return "RGB24";
        default: {
            char buf[5] = {};
            buf[0] = char((fcc >> 24) & 0xFF);
            buf[1] = char((fcc >> 16) & 0xFF);
            buf[2] = char((fcc >>  8) & 0xFF);
            buf[3] = char( fcc        & 0xFF);
            return {buf, 4};
        }
    }
}

// ── AVFoundation delegate ─────────────────────────────────────────────────────

@interface PMVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@end

@implementation PMVideoDelegate {
    std::function<void(const pm::video::VideoFrame&)>* _cb;
    std::atomic<bool>* _running;
    int* _actual_w;
    int* _actual_h;
}

- (instancetype)initWithCallback:(std::function<void(const pm::video::VideoFrame&)>*)cb
                         running:(std::atomic<bool>*)running
                        actualW:(int*)aw
                        actualH:(int*)ah {
    if ((self = [super init])) {
        _cb      = cb;
        _running = running;
        _actual_w = aw;
        _actual_h = ah;
    }
    return self;
}

- (void)captureOutput:(AVCaptureOutput*)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection*)connection {
    if (!_running || !_running->load()) return;

    CVImageBufferRef buf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!buf) return;

    CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

    int w = (int)CVPixelBufferGetWidth(buf);
    int h = (int)CVPixelBufferGetHeight(buf);
    size_t stride = CVPixelBufferGetBytesPerRow(buf);
    const uint8_t* base = (const uint8_t*)CVPixelBufferGetBaseAddress(buf);

    pm::video::VideoFrame frame;
    frame.width  = w;
    frame.height = h;
    frame.format = pm::video::PixelFormat::BGR24;

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    frame.timestamp_ms = (int64_t)(CMTimeGetSeconds(pts) * 1000.0);

    if (base && w > 0 && h > 0) {
        // We request kCVPixelFormatType_32BGRA (universally supported conversion target).
        // Pack it tightly into BGR24, dropping the unused alpha byte.
        frame.stride = w * 3;
        frame.data.resize((size_t)w * (size_t)h * 3);
        uint8_t* dst = frame.data.data();
        for (int row = 0; row < h; ++row) {
            const uint8_t* src = base + row * stride;
            for (int col = 0; col < w; ++col) {
                dst[0] = src[0]; // B
                dst[1] = src[1]; // G
                dst[2] = src[2]; // R
                src += 4;        // skip A
                dst += 3;
            }
        }
    }

    CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

    if (_actual_w && *_actual_w == 0) {
        *_actual_w = w;
        *_actual_h = h;
    }

    if (_cb && *_cb) (*_cb)(frame);
}

@end

// ── PMAssetWriterCapture — feeds sample buffers into AVAssetWriterInput ───────

@interface PMAssetWriterCapture : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, strong) AVAssetWriter*      writer;
@property (nonatomic, strong) AVAssetWriterInput* videoInput;
@property (atomic,   assign)  BOOL                started;
@property (atomic,   assign)  BOOL                stopRequested;
@property (atomic,   assign)  int                 frameCount;
@end

@implementation PMAssetWriterCapture

- (void)captureOutput:(AVCaptureOutput*)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection*)connection {
    if (self.stopRequested) return;
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    if (!self.started) {
        [self.writer startSessionAtSourceTime:pts];
        self.started = YES;
    }
    if (self.videoInput.isReadyForMoreMediaData) {
        if ([self.videoInput appendSampleBuffer:sampleBuffer])
            self.frameCount++;
    }
}

@end

// ── pm::video implementation ──────────────────────────────────────────────────

namespace pm::video {

// ── enumerate_capture_devices ─────────────────────────────────────────────────

std::vector<DeviceInfo> enumerate_capture_devices() {
    @autoreleasepool {
        NSArray<AVCaptureDevice*>* devs = av_video_devices();
        AVCaptureDevice* def = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        NSString* def_id = def ? def.uniqueID : nil;

        std::vector<DeviceInfo> result;
        result.reserve([devs count]);
        for (AVCaptureDevice* d in devs) {
            DeviceInfo info;
            info.id         = ns_to_std(d.uniqueID);
            info.name       = ns_to_std(d.localizedName);
            info.is_default = def_id && [d.uniqueID isEqualToString:def_id];
            result.push_back(std::move(info));
        }
        return result;
    }
}

// ── enumerate_device_modes ────────────────────────────────────────────────────

std::vector<DeviceMode> enumerate_device_modes(const std::string& device_name) {
    @autoreleasepool {
        NSArray<AVCaptureDevice*>* devs = av_video_devices();
        AVCaptureDevice* target = find_av_device(devs, device_name);

        if (!target) return {};

        std::vector<DeviceMode> result;
        for (AVCaptureDeviceFormat* fmt in target.formats) {
            CMFormatDescriptionRef desc = fmt.formatDescription;
            CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(desc);
            FourCharCode fcc = CMFormatDescriptionGetMediaSubType(desc);

            bool is_jpeg = (fcc == kCMVideoCodecType_JPEG ||
                            fcc == kCMVideoCodecType_JPEG_OpenDML);
            std::string fmt_str = is_jpeg ? "MJPEG" : fourcc_name(fcc);

            for (AVFrameRateRange* rr in fmt.videoSupportedFrameRateRanges) {
                DeviceMode mode;
                mode.width   = (int)dim.width;
                mode.height  = (int)dim.height;
                mode.fps_num = (int)(rr.maxFrameRate + 0.5);
                mode.fps_den = 1;
                mode.format  = fmt_str;
                result.push_back(mode);
            }
        }
        return result;
    }
}

// ── VideoInput::Impl ──────────────────────────────────────────────────────────

struct VideoInput::Impl {
    AVCaptureSession*         session_   = nil;
    AVCaptureDeviceInput*     devInput_  = nil;
    AVCaptureVideoDataOutput* vidOutput_ = nil;
    PMVideoDelegate*          delegate_  = nil;
    dispatch_queue_t          queue_     = nullptr;

    std::atomic<bool> running_{false};
    std::string       device_name_;
    int               actual_w_ = 0;
    int               actual_h_ = 0;

    std::function<void(const VideoFrame&)> callback_;

    Impl()  = default;
    ~Impl() { if (running_.load()) stop(); }

    void start(const std::string& device_name, int width, int height, int fps,
               std::function<void(const VideoFrame&)> cb) {
        @autoreleasepool {
            callback_ = std::move(cb);

            // --- TCC camera permission ---
            AVAuthorizationStatus status =
                [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
            if (status == AVAuthorizationStatusNotDetermined) {
                // Block until the user responds to the system prompt.
                dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                    completionHandler:^(BOOL granted) {
                        dispatch_semaphore_signal(sem);
                    }];
                dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
                status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
            }
            if (status != AVAuthorizationStatusAuthorized) {
                throw std::runtime_error(
                    "video: camera access denied — grant permission in "
                    "System Settings → Privacy & Security → Camera");
            }

            // --- find device ---
            NSArray<AVCaptureDevice*>* devs = av_video_devices();
            AVCaptureDevice* dev = find_av_device(devs, device_name);
            if (!dev)
                throw std::runtime_error(
                    "video: device not found: " +
                    (device_name.empty() ? "(default)" : device_name));

            device_name_ = ns_to_std(dev.localizedName);

            // --- choose format / frame-rate ---
            if ((width > 0 || height > 0 || fps > 0) &&
                [dev lockForConfiguration:nil]) {
                AVCaptureDeviceFormat* best = nil;
                double best_diff = 1e18;
                for (AVCaptureDeviceFormat* fmt in dev.formats) {
                    CMVideoDimensions dim =
                        CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
                    double dw = width  > 0 ? std::abs(dim.width  - width)  : 0;
                    double dh = height > 0 ? std::abs(dim.height - height) : 0;
                    double d  = dw * dw + dh * dh;
                    if (d < best_diff) { best_diff = d; best = fmt; }
                }
                if (best) {
                    dev.activeFormat = best;
                    if (fps > 0) {
                        CMTime ft = CMTimeMake(1, fps);
                        dev.activeVideoMinFrameDuration = ft;
                        dev.activeVideoMaxFrameDuration = ft;
                    }
                }
                [dev unlockForConfiguration];
            }

            // --- build session ---
            session_ = [AVCaptureSession new];
            [session_ beginConfiguration];

            NSError* err = nil;
            devInput_ = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
            if (!devInput_)
                throw std::runtime_error(
                    "video: cannot open device: " +
                    ns_to_std(err.localizedDescription));

            if ([session_ canAddInput:devInput_])
                [session_ addInput:devInput_];

            vidOutput_ = [AVCaptureVideoDataOutput new];
            vidOutput_.alwaysDiscardsLateVideoFrames = YES;
            // kCVPixelFormatType_32BGRA is always available as a conversion target;
            // the delegate strips alpha to produce packed BGR24 frames.
            vidOutput_.videoSettings = @{
                (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)
            };

            queue_    = dispatch_queue_create("pm.video.capture", DISPATCH_QUEUE_SERIAL);
            delegate_ = [[PMVideoDelegate alloc] initWithCallback:&callback_
                                                          running:&running_
                                                          actualW:&actual_w_
                                                          actualH:&actual_h_];
            [vidOutput_ setSampleBufferDelegate:delegate_ queue:queue_];

            if ([session_ canAddOutput:vidOutput_])
                [session_ addOutput:vidOutput_];

            [session_ commitConfiguration];
            running_.store(true);
            [session_ startRunning];
        }
    }

    void stop() {
        running_.store(false);
        @autoreleasepool {
            if (session_) { [session_ stopRunning]; session_ = nil; }
            delegate_  = nil;
            vidOutput_ = nil;
            devInput_  = nil;
            queue_     = nullptr;
        }
    }
};

// ── VideoInput public API ─────────────────────────────────────────────────────

VideoInput::VideoInput()  : m_(std::make_unique<Impl>()) {}
VideoInput::~VideoInput() = default;

void VideoInput::start(const std::string& device_name, int width, int height, int fps,
                       std::function<void(const VideoFrame&)> on_frame) {
    m_->start(device_name, width, height, fps, std::move(on_frame));
}

void VideoInput::stop()             { m_->stop(); }
bool VideoInput::is_running() const { return m_->running_.load(); }
const std::string& VideoInput::opened_device_name() const { return m_->device_name_; }
int VideoInput::actual_width()  const { return m_->actual_w_; }
int VideoInput::actual_height() const { return m_->actual_h_; }

// ── capture_still ─────────────────────────────────────────────────────────────

VideoFrame capture_still(const std::string& device_name, int width, int height,
                         int timeout_ms, std::function<bool()> cancel_fn) {
    std::mutex              mu;
    std::condition_variable cv;
    VideoFrame              frame;
    bool                    got = false;

    VideoInput vin;
    vin.start(device_name, width, height, 0,
              [&](const VideoFrame& f) {
                  std::lock_guard<std::mutex> lk(mu);
                  if (!got) { frame = f; got = true; cv.notify_one(); }
              });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lk(mu);
    while (!got) {
        if (cancel_fn && cancel_fn()) {
            vin.stop();
            throw std::runtime_error("video: cancelled");
        }
        if (cv.wait_until(lk, std::min(deadline, std::chrono::steady_clock::now() +
                                                  std::chrono::milliseconds(80))) == std::cv_status::timeout) {
            if (std::chrono::steady_clock::now() >= deadline) {
                vin.stop();
                throw std::runtime_error("video: timeout waiting for frame");
            }
        }
    }
    vin.stop();
    return frame;
}

// ── encode_jpeg ───────────────────────────────────────────────────────────────

std::vector<uint8_t> encode_jpeg(const VideoFrame& frame, int quality) {
    if (frame.data.empty() || frame.width <= 0 || frame.height <= 0)
        return {};

    @autoreleasepool {
        // BGR24 → RGB24 swizzle for ImageIO.
        size_t bpr  = (frame.stride > 0) ? (size_t)frame.stride : (size_t)frame.width * 3;
        size_t pix  = (size_t)frame.width * (size_t)frame.height;
        std::vector<uint8_t> rgb(pix * 3);
        for (size_t row = 0; row < (size_t)frame.height; ++row) {
            for (size_t col = 0; col < (size_t)frame.width; ++col) {
                size_t src = row * bpr + col * 3;
                size_t dst = (row * (size_t)frame.width + col) * 3;
                rgb[dst + 0] = frame.data[src + 2]; // R
                rgb[dst + 1] = frame.data[src + 1]; // G
                rgb[dst + 2] = frame.data[src + 0]; // B
            }
        }

        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, rgb.data(), rgb.size(), nullptr);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef img = CGImageCreate(
            (size_t)frame.width, (size_t)frame.height,
            8, 24, (size_t)frame.width * 3,
            cs, kCGBitmapByteOrderDefault | kCGImageAlphaNone,
            provider, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(provider);
        if (!img) return {};

        NSMutableData* out = [NSMutableData data];
        CGImageDestinationRef dest = CGImageDestinationCreateWithData(
            (__bridge CFMutableDataRef)out, PM_UTI_JPEG, 1, nullptr);
        if (!dest) { CGImageRelease(img); return {}; }

        float q = (float)std::max(1, std::min(quality, 100)) / 100.0f;
        NSDictionary* props = @{
            (NSString*)kCGImageDestinationLossyCompressionQuality: @(q)
        };
        CGImageDestinationAddImage(dest, img, (__bridge CFDictionaryRef)props);
        bool ok = CGImageDestinationFinalize(dest);
        CFRelease(dest);
        CGImageRelease(img);
        if (!ok) return {};

        std::vector<uint8_t> jpeg(out.length);
        memcpy(jpeg.data(), out.bytes, out.length);
        return jpeg;
    }
}

// ── save_frame ────────────────────────────────────────────────────────────────

bool save_frame(const VideoFrame& frame, const std::string& path,
                std::string& err, int jpeg_quality) {
    @autoreleasepool {
        auto jpeg = encode_jpeg(frame, jpeg_quality);
        if (jpeg.empty()) { err = "encode_jpeg failed"; return false; }

        NSData* data = [NSData dataWithBytes:jpeg.data() length:jpeg.size()];
        NSError* ns_err = nil;
        if (![data writeToFile:std_to_ns(path)
                       options:NSDataWritingAtomic
                         error:&ns_err]) {
            err = ns_to_std(ns_err.localizedDescription);
            return false;
        }
        return true;
    }
}

bool load_frame(const std::string& /*path*/, VideoFrame& /*out*/, std::string& err) {
    err = "load_frame: not implemented on macOS yet";
    return false;
}

void record_movie(const std::string& device_name,
                  int width, int height, int fps,
                  const std::string& path,
                  int duration_ms,
                  std::function<bool()>             stop_fn,
                  std::function<void(int64_t, int)> on_status) {
    @autoreleasepool {
        // --- TCC permission ---
        AVAuthorizationStatus status =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (status == AVAuthorizationStatusNotDetermined) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                completionHandler:^(BOOL) { dispatch_semaphore_signal(sem); }];
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        }
        if (status != AVAuthorizationStatusAuthorized)
            throw std::runtime_error(
                "video: camera access denied — grant permission in "
                "System Settings → Privacy & Security → Camera");

        // --- find device ---
        NSArray<AVCaptureDevice*>* devs = av_video_devices();
        AVCaptureDevice* dev = find_av_device(devs, device_name);
        if (!dev)
            throw std::runtime_error("video: device not found: " +
                                     (device_name.empty() ? "(default)" : device_name));

        // --- pick best format (highest pixel count, prefer NV12/BGRA uncompressed) ---
        if ([dev lockForConfiguration:nil]) {
            AVCaptureDeviceFormat* best   = nil;
            int    best_pixels = 0;
            int    best_dist   = INT_MAX; // used when caller specifies size
            double best_fps    = 0;
            bool   want_size   = (width > 0 || height > 0);

            for (AVCaptureDeviceFormat* fmt in dev.formats) {
                CMFormatDescriptionRef desc = fmt.formatDescription;
                FourCharCode fcc = CMFormatDescriptionGetMediaSubType(desc);
                // Prefer uncompressed/planar — skip MJPEG/HEVC encoded formats.
                bool ok = (fcc == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                           fcc == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange  ||
                           fcc == kCVPixelFormatType_32BGRA);
                if (!ok) continue;

                CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(desc);
                int pixels = (int)dim.width * (int)dim.height;
                double max_rng_fps = 0;
                for (AVFrameRateRange* r in fmt.videoSupportedFrameRateRanges)
                    max_rng_fps = std::max(max_rng_fps, r.maxFrameRate);

                if (want_size) {
                    int dw   = width  > 0 ? std::abs((int)dim.width  - width)  : 0;
                    int dh   = height > 0 ? std::abs((int)dim.height - height) : 0;
                    int dist = dw * dw + dh * dh;
                    if (dist < best_dist) { best_dist = dist; best = fmt; }
                } else {
                    // No size preference → highest resolution wins.
                    if (pixels > best_pixels ||
                        (pixels == best_pixels && max_rng_fps > best_fps)) {
                        best_pixels = pixels;
                        best_fps    = max_rng_fps;
                        best        = fmt;
                    }
                }
            }

            if (best) {
                dev.activeFormat = best;
                int target_fps = fps > 0 ? fps : 30;
                for (AVFrameRateRange* rr in best.videoSupportedFrameRateRanges) {
                    if (rr.minFrameRate <= target_fps && target_fps <= rr.maxFrameRate) {
                        CMTime ft = CMTimeMake(1, target_fps);
                        dev.activeVideoMinFrameDuration = ft;
                        dev.activeVideoMaxFrameDuration = ft;
                        break;
                    }
                }
            }
            [dev unlockForConfiguration];
        }

        // Actual dimensions after format lock.
        CMVideoDimensions dim2 =
            CMVideoFormatDescriptionGetDimensions(dev.activeFormat.formatDescription);
        int actual_w   = (int)dim2.width;
        int actual_h   = (int)dim2.height;
        int actual_fps = fps > 0 ? fps : 30;

        // --- AVAssetWriter (MPEG-4 / H.264) -----------------------------------
        NSURL* url = [NSURL fileURLWithPath:std_to_ns(path)];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

        NSError* err = nil;
        AVAssetWriter* writer =
            [AVAssetWriter assetWriterWithURL:url
                                     fileType:AVFileTypeMPEG4
                                        error:&err];
        if (!writer)
            throw std::runtime_error("video: AVAssetWriter init failed: " +
                                     ns_to_std(err.localizedDescription));

        // H.264 High-profile; bitrate ~5% of raw pixels/frame.
        int64_t bitrate = (int64_t)actual_w * actual_h * actual_fps / 20;
        AVAssetWriterInput* videoInput =
            [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                outputSettings:@{
                    AVVideoCodecKey:  AVVideoCodecTypeH264,
                    AVVideoWidthKey:  @(actual_w),
                    AVVideoHeightKey: @(actual_h),
                    AVVideoCompressionPropertiesKey: @{
                        AVVideoAverageBitRateKey:          @(bitrate),
                        AVVideoProfileLevelKey:            AVVideoProfileLevelH264HighAutoLevel,
                        AVVideoH264EntropyModeKey:         AVVideoH264EntropyModeCABAC,
                        AVVideoExpectedSourceFrameRateKey: @(actual_fps),
                        AVVideoMaxKeyFrameIntervalKey:     @(actual_fps * 2),
                    }
                }];
        videoInput.expectsMediaDataInRealTime = YES;
        [writer addInput:videoInput];

        // --- capture session --------------------------------------------------
        AVCaptureSession* session = [AVCaptureSession new];
        [session beginConfiguration];

        AVCaptureDeviceInput* devInput =
            [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
        if (!devInput)
            throw std::runtime_error("video: cannot open device: " +
                                     ns_to_std(err.localizedDescription));
        [session addInput:devInput];

        AVCaptureVideoDataOutput* dataOut = [AVCaptureVideoDataOutput new];
        dataOut.alwaysDiscardsLateVideoFrames = YES;
        // NV12 full-range — camera-native, no pixel conversion needed.
        dataOut.videoSettings = @{
            (NSString*)kCVPixelBufferPixelFormatTypeKey:
                @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        };

        PMAssetWriterCapture* cap = [PMAssetWriterCapture new];
        cap.writer     = writer;
        cap.videoInput = videoInput;

        dispatch_queue_t captureQ =
            dispatch_queue_create("pm.video.record", DISPATCH_QUEUE_SERIAL);
        [dataOut setSampleBufferDelegate:cap queue:captureQ];
        [session addOutput:dataOut];
        [session commitConfiguration];

        [writer startWriting];
        [session startRunning];

        // --- run loop --------------------------------------------------------
        // Pump CFRunLoop so AVFoundation callbacks fire on this thread.
        auto t_start       = std::chrono::steady_clock::now();
        auto t_last_status = t_start;

        for (;;) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, YES);
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - t_start).count();

            if ((duration_ms > 0 && elapsed >= duration_ms) ||
                (stop_fn && stop_fn())) break;

            if (on_status) {
                auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - t_last_status).count();
                if (since >= 1000) {
                    t_last_status = now;
                    on_status(elapsed, cap.frameCount);
                }
            }
        }

        // --- finalize --------------------------------------------------------
        cap.stopRequested = YES;
        [session stopRunning];
        [videoInput markAsFinished];

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSError* finishErr = nil;
        [writer finishWritingWithCompletionHandler:^{
            finishErr = writer.error;
            dispatch_semaphore_signal(done);
        }];
        // Pump run loop while the encoder flushes.
        // Also honour a second Ctrl+C (cancel during finalization) — the MP4
        // moov atom won't be written but the file is at least not orphaned.
        for (;;) {
            if (dispatch_semaphore_wait(done,
                    dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) == 0)
                break; // finalization complete
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, YES);
            if (media::cli::cancel_requested()) break; // second Ctrl+C
        }

        if (finishErr)
            throw std::runtime_error("video record_movie: finish failed: " +
                                     ns_to_std(finishErr.localizedDescription));
    }
}

} // namespace pm::video

#endif // FEATURE_VIDEO
