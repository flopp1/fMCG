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

// Input limits: beyond these a typo makes trillions of frames.
constexpr double k_max_fps = 1000.0;
constexpr double k_max_delay = 3600.0;   // seconds (start and end delay)

// Every UI setting: the persisted globals, the pattern being edited and the
// per-session paths. The Process worker gets a copy of it.
struct GuiSettings {
    std::string    midi_file;
    std::string    output_video;
    GlobalSettings global;            // saved in settings.ini
    PatternData    pattern;           // the pattern being edited (layout_text = the editor's content)
    bool           risky_dedup = false;   // risky fast dedup (needs layered); deliberately NOT saved
};

// Options snapshot handed to the render worker thread.
struct RenderSettings {
    std::string output_video;
    std::string midi_dir;
    std::string midi_stem;
    int width, height;
    double fps, total_duration;
    int ffmpeg_threads = 0;           // encoder threads (x264), 0 = auto
    std::string bg_color_aabbggrr = "&H00000000";
    int  output_format = 0;           // 0 = MP4 (H.264), 1 = MOV (qtrle)
    bool transparent_bg = false;      // MOV only
    int         x264_crf = 18;               // global encoder settings (MP4): CRF 0..51
    std::string x264_preset = "veryfast";    // fmcg_video::k_presets
    std::string x264_tune;                   // "" = none, else fmcg_video::k_tunes
    double      keyframe_seconds = 2.0;      // both formats
    FrameTextSource       text_source;       // frames + pattern, snapped at the Render click
    fmcg_text::TextStyle  text_style;
    fmcg_text::Placement  placement;
    std::string           note;              // logged at the start ("" = none)
};

struct AppState {
    // ---- operation lifecycle -------------------------------------------------
    std::mutex                log_mutex;
    std::vector<std::string>  log_lines;
    std::atomic<float>        progress{0.0f};
    std::atomic<float>        render_speed{0.0f};   // render speed (x realtime), 0 = unknown
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
    std::string               midi_dir;
    std::string               midi_stem;
    double                    total_duration{0.0};
    double                    fps{60.0};
    int                       vid_width{1920};
    int                       vid_height{1080};
    uint16_t                  ppqn{480};
    uint64_t                  total_notes{0};
    uint64_t                  total_ticks{0};
    SongMeter                 meter;               // beat / measure totals
    GuiSettings               processed_with;      // the settings the last Process ran with
    uint64_t                  frames_epoch{0};     // bumped per completed Process (preview cache key)

    // ---- preview data ---------------------------------------------------------
    std::vector<FrameStats>   frames_data;
    // The overlay look the preview and the render draw with: the pattern the
    // last Process ran with, updated from the live UI at each Render click
    // (snap_live_settings). template_lines = layout_lines(pattern.layout_text).
    PatternData               pattern;
    std::vector<std::string>  template_lines;
    bool                      show_preview{false};
    bool                      render_active{false};  // true while a render is in progress
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
    double                    end_delay{0.0};     // frozen-stats tail after the song ends
    double                    preview_start_time{0.0};
    double                    preview_start_pos{0.0};

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
    bool                      scan_has_tracks = false;   // the running scan reports tracks
    uint64_t                  scan_tracks = 0;           // tracks finished so far
    uint64_t                  scan_tracks_total = 0;     // 0 = unknown

    // ---- methods ---------------------------------------------------------------
    void gui_log(const char* msg, bool is_error);
    void gui_progress(const char* pass_name, int percent);
    void gui_scan_progress(uint64_t events, double elapsed_sec, double ev_per_s, double frac);
    void gui_scan_tracks(uint64_t done, uint64_t total);
    std::string compose_scan_line();     // caller holds scan_mutex
    void finish_op();                    // end-of-operation bookkeeping
    float display_progress();            // smooth progress fraction for the bar
};

// All UI-editable state, hoisted out of main() so the pattern manager,
// colour editor and persistence layer can read/write it alongside the
// settings widgets.
struct UiState {
    GuiSettings     s;              // every setting (globals, pattern, paths)
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
// 3x3 anchor index in reading order (0 = top-left .. 8 = bottom-right) -> numpad 7 8 9 / 4 5 6 / 1 2 3.
int gui_anchor_to_numpad(int idx);

// In-built renderer inputs built from the processed/preview state in g_app
// (shared by the preview and the render job so both draw identical frames).
fmcg_text::TextStyle app_text_style();
fmcg_text::Placement app_placement();
FrameTextSource app_text_source();

// Set the preview / render look (g_app.pattern + its template lines).
void set_app_pattern(const PatternData& p);
