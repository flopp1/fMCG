// fmcg_video.h -- in-built video output: encoders + ISO-BMFF muxer.
//   MP4  : H.264 via x264 (default)            -- plays everywhere
//   MOV  : QuickTime Animation (qtrle), lossless, optional alpha -- for editors
// Frames come from fmcg_text::FrameRenderer canvases; `dirty` is the area that
// may differ from the previous frame (everything outside it is unchanged), so
// both encoders only touch that area.
#pragma once
#include "fmcg_text.h"
#include "fmcg_format.h"

#include <atomic>
#include <functional>

#include <cstdint>
#include <memory>
#include <string>

namespace fmcg_video {

enum class Format { MP4_H264 = 0, MOV_QTRLE = 1 };

struct Options {
    Format format = Format::MP4_H264;
    int width = 1920, height = 1080;
    double fps = 60.0;
    bool transparent = false;   // MOV only: 32-bit ARGB with the canvas alpha
    int threads = 0;            // x264 threads, 0 = auto
    int crf = 18;               // x264 constant-quality factor
    std::string preset = "veryfast";
    double keyframe_seconds = 2.0;   // keyframe interval (x264 max GOP / qtrle full frames)
};

// File extension for a format, with the dot.
const char* extension(Format f);

class Writer {
public:
    virtual ~Writer() = default;
    // Appends one frame. `dirty` bounds every pixel that may differ from the
    // previous frame (the first frame is always encoded whole).
    virtual bool add_frame(const fmcg_text::Canvas& frame, fmcg_text::Rect dirty) = 0;
    // Flushes the encoder and writes the index; the file is complete after this.
    virtual bool finish() = 0;
    // Stops and deletes the partial file.
    virtual void abort() = 0;
    virtual const std::string& error() const = 0;
    virtual uint64_t bytes_written() const = 0;
};

// nullptr on failure (err filled). Width/height must be even for MP4 (4:2:0).
std::unique_ptr<Writer> create_writer(const std::string& path, const Options& opt, std::string* err);

// Renders every video frame of `text` (lead-in, song, tail) with the given
// style/placement/background and encodes it to `path`. `progress(done, total)`
// is called at most every ~100 ms. On cancel or error the partial file is
// deleted. Returns 0 = ok, -2 = cancelled, -1 = error (err filled).
int render_video(const std::string& path, const Options& opt, const FrameTextSource& text,
                 const fmcg_text::TextStyle& style, const fmcg_text::Placement& placement,
                 fmcg_text::RGBA background, const std::atomic<bool>* cancel,
                 const std::function<void(size_t done, size_t total)>& progress, std::string* err);

} // namespace fmcg_video
