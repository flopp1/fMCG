// app_state.h -- shared GUI state: operation lifecycle, processed data,
// preview mirror, and the live scan-progress snapshot.
//
// Everything here is written from worker threads (process/render/dialog) and
// read from the UI thread, so mutable cross-thread fields are atomic or
// mutex-guarded.
#pragma once
#include "fMCG_core.h"
#include "settings_store.h"   // GlobalSettings, PatternData

#include <imgui.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Options snapshot handed to the processing worker thread.
struct GuiSettings {
    std::string midi_file;
    std::string output_video;
    std::string layout_text;
    std::string font_family;
    std::string font_variant;   // subfamily/style within the family ("" = default)
    int         font_bold = 0;
    int         font_italic = 0;
    int         font_size = 36;
    int         alignment = 1;
    int         width = 1920;
    int         height = 1080;
    double      fps = 60.0;
    CommaOpts   commas;
    PadOpts     pad;
    BpmOpts     bpm;
    bool        cc_stats = false;
    double      start_delay = 3.0;
    // 0 = corner alignment dropdown, 1 = explicit x/y (top-left of text block)
    int         pos_mode = 0;
    int         pos_x = 30;
    int         pos_y = 30;
    bool        vel0_note_off = true;
    int         ffmpeg_threads = 0;   // 0 = ffmpeg auto-selects (no -threads flag)
    std::string text_color_aabbggrr = "&H00FFFFFF";
    std::string bg_color_aabbggrr = "&H00000000";
};

// Options snapshot handed to the render worker thread.
struct RenderSettings {
    std::string output_video;
    std::string midi_dir;
    std::string midi_stem;
    int width, height;
    double fps, total_duration;
    int ffmpeg_threads = 0;           // 0 = ffmpeg auto-selects (no -threads flag)
    std::string bg_color_aabbggrr = "&H00000000";
};

struct AppState {
    // ---- operation lifecycle -------------------------------------------------
    std::mutex                log_mutex;
    std::vector<std::string>  log_lines;
    std::atomic<float>        progress{0.0f};
    std::atomic<float>        render_speed{0.0f};   // ffmpeg-reported encode speed (x realtime), 0 = unknown
    std::atomic<double>       render_wallclock{0.0};// render wall-clock seconds (for effective speed)
    std::atomic<bool>         busy{false};
    std::atomic<bool>         done{false};
    std::atomic<bool>         cancel{false};   // user-requested abort of process/render
    std::string               result_path;
    std::atomic<int>          result_ret{-1};

    // What the last completed operation was and how it ended. Kept separate
    // from result_ret (which carries process/ffmpeg exit codes) so the status
    // line can say "Processing failed" vs "Render failed" vs "...cancelled"
    // instead of lumping every non-zero result under "Render failed".
    enum OpResult {
        OP_NONE = 0,            // no completed operation yet (fresh/idle)
        OP_PROCESS_OK, OP_PROCESS_FAIL, OP_PROCESS_CANCELLED,
        OP_RENDER_OK,   OP_RENDER_FAIL,   OP_RENDER_CANCELLED
    };
    std::atomic<int>          op_result{OP_NONE};

    // ---- processed state ------------------------------------------------------
    std::atomic<bool>         processed{false};
    std::string               ass_path;
    std::string               midi_dir;
    std::string               midi_stem;
    double                    total_duration{0.0};
    double                    fps{60.0};
    int                       vid_width{1920};
    int                       vid_height{1080};
    uint16_t                  ppqn{480};
    uint64_t                  total_notes{0};

    // ---- preview data ---------------------------------------------------------
    std::vector<FrameStats>   frames_data;
    std::string               layout_text;         // template editor content (persistent)
    std::vector<std::string>  template_lines;
    std::string               text_colour_ass{"&H00FFFFFF"};
    int                       ass_alignment{7};
    int                       font_size{36};       // video-resolution font size (ASS Fontsize)
    std::string               font_family;         // family used for the last processed render
    std::string               font_variant;        // subfamily/style, e.g. "Bold"
    int                       font_bold = 0;
    int                       font_italic = 0;
    bool                      show_preview{false};
    bool                      render_active{false};  // true while FFmpeg render is in progress
    // The Preview popup was opened to watch a render (Render Video click, or
    // Preview while a render runs). Such a popup closes itself when the render
    // ends instead of degrading into a regular timeline preview.
    bool                      preview_watching_render{false};
    // One-shot latch set by the render worker when the render finishes (before
    // finish_op clears render_active/busy, whose simultaneous write would
    // otherwise race away the "render just ended" window the popup watches
    // for). The preview consumes it to close a watching popup.
    std::atomic<bool>         render_watch_done{false};

    // MIDI spec-violation prompt: the processing worker hits a tick beyond the
    // spec's 28-bit delta limit, asks the user (proceed / two-pass / cancel)
    // through this flag-and-answer pair, and blocks until the UI answers.
    std::atomic<bool>         spec_prompt_open{false};
    std::atomic<int>          spec_choice{-1};      // -1 pending, 0 proceed, 1 two-pass, 2 cancel
    bool                      spec_popup_started{false};   // UI-thread only: OpenPopup once
    bool                      preview_playing{false};
    double                    preview_time{0.0};
    // Where the NORMAL (timeline) preview last was, saved on every update of
    // preview_time outside the render view. The render view drives
    // preview_time itself (it is the render progress) and must not destroy
    // this; on render completion the timeline resumes from here.
    double                    preview_saved_pos{0.0};
    double                    start_delay{0.0};   // black lead-in before the song starts
    int                       pos_mode{0};        // 0 = corners, 1 = explicit x/y
    int                       pos_x{30};
    int                       pos_y{30};
    int                       alignment{1};       // corner dropdown (GUI index)
    double                    preview_start_time{0.0};
    double                    preview_start_pos{0.0};

    // Preview font (same TTF family as the rendered video, drawn at a size
    // scaled to match: preview_px = font_size * preview_width / video_width)
    ImFont*                   preview_font{nullptr};
    std::string               preview_baked_family;
    std::atomic<bool>         preview_font_reload{false};

    // Preview mirror of the options the last Process ran with.
    CommaOpts                 preview_commas;
    PadOpts                   preview_pad;
    BpmOpts                   preview_bpm;
    std::string               bg_colour_ass{"&H00000000"};

    // ---- async file dialog state ---------------------------------------------
    std::atomic<bool>         dialog_busy{false};
    std::atomic<bool>         dialog_done{false};
    std::string               dialog_result;
    std::string               dialog_kind;   // which UI element opened the dialog

    // ---- live scan-progress snapshot (written by the processing thread) -------
    std::mutex                scan_mutex;
    uint64_t                  scan_events = 0;
    double                    scan_elapsed = 0.0;
    double                    scan_evps = 0.0;
    double                    scan_frac = -1.0;   // stream fraction 0..1, <0 unknown
    bool                      scan_active = false;

    // ---- methods ---------------------------------------------------------------
    void gui_log(const char* msg, bool is_error);
    void gui_progress(const char* pass_name, int percent);
    void gui_scan_progress(uint64_t events, double elapsed_sec, double ev_per_s, double frac);
    std::string compose_scan_line();     // caller holds scan_mutex
    void finish_op();                    // end-of-operation bookkeeping
    float display_progress();            // smooth progress fraction for the bar
};

// All UI-editable state, hoisted out of main() so the pattern manager,
// colour editor and persistence layer can read/write it alongside the
// settings widgets.
struct UiState {
    GuiSettings     s;              // snapshot handed to the Process worker
    GlobalSettings  globals;        // non-pattern persistent settings
    bool            globals_dirty = false;      // autosave pending
    double          last_globals_save = 0.0;    // seconds (glfwGetTime)

    // editing buffers (ImGui InputText needs char arrays)
    char midi_buf[1024] = "";
    char output_buf[1024] = "";
    char layout_buf[8192] = "";
    int  alignment_idx = 0;         // corner/center dropdown index
    int  selected_font_idx = 0;
    std::vector<std::string> system_fonts;
    std::vector<const char*> font_cstrs;        // refreshed after list changes

    // font-variant combo cache (rebuilt when the family changes)
    // NOTE: never cache const char* into these strings across frames -- the
    // rebuild reallocates the vector and dangles every earlier pointer (the
    // combo then displays freed heap). The cstr array is rebuilt locally each
    // frame in main.cpp right before ImGui::Combo consumes it.
    std::vector<std::string> variant_names;
    std::string variants_for;
    int  variant_idx = 0;

    // pattern management
    std::vector<std::string> pattern_names;
    int          active_pattern_idx = 0;
    std::string  active_pattern;    // "" = unsaved new pattern
    PatternData  pattern_baseline;  // last loaded/saved state (modified check)
    bool         show_save_as = false;
    bool         show_switch_confirm = false;
    std::string  pending_switch_to;
    char         save_as_name[128] = "";
    bool         save_as_warn = false;
};
extern UiState g_ui;   // single instance (defined in app_state.cpp)

extern AppState g_app;   // single global instance (defined in app_state.cpp)

// GUI corner dropdown index -> ASS \an numpad alignment
// (0 TL,1 TR,2 BL,3 BR,4 Top Center,5 Bottom Center -> 7,9,1,3,8,2)
int gui_alignment_to_ass(int idx);

// Recompute font_bold/font_italic from the variant's OS/2 metrics and refresh
// the variant combo cache for `family`. Shared by the family combo and the
// pattern loader.
void apply_font_variant(GuiSettings& s, const std::string& family, const std::string& variant);
