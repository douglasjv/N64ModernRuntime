#include <thread>
#include <atomic>
#include <array>
#include <chrono>
#include <cinttypes>
#include <variant>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <optional>
#include <queue>
#include <cstring>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/extensions.h"

#include "ultramodern/rsp.hpp"
#include "ultramodern/renderer_context.hpp"

#if defined(__ANDROID__) && defined(RECOMP_ENABLE_ANDROID_TRACE_LOGS)
#include <android/log.h>
#define ULTRAMODERN_ANDROID_RENDER_LOG(...) __android_log_print(ANDROID_LOG_INFO, "UltraRender", __VA_ARGS__)
#else
#define ULTRAMODERN_ANDROID_RENDER_LOG(...) ((void)0)
#endif

static ultramodern::events::callbacks_t events_callbacks{};

void ultramodern::events::set_callbacks(const ultramodern::events::callbacks_t& callbacks) {
    events_callbacks = callbacks;
}

struct SpTaskAction {
    OSTask task;
};

struct ScreenUpdateAction {
    ultramodern::renderer::ViRegs regs;
};

struct UpdateConfigAction {
};

struct DummyWorkloadAction {
    int32_t fb_address;
};

using Action = std::variant<SpTaskAction, ScreenUpdateAction, UpdateConfigAction, DummyWorkloadAction>;

struct ViState {
    const OSViMode* mode;
    PTR(void) framebuffer;
    PTR(OSMesg) mq;
    OSMesg msg;
    uint32_t state;
    uint32_t control;
    int retrace_count = 1;
};

#define VI_STATE_BLACK 0x20
#define VI_STATE_REPEATLINE 0x40

static struct {
    struct {
        std::thread thread;
        int cur_state;
        int field;
        ViState states[2];
        ultramodern::renderer::ViRegs regs;
        ultramodern::renderer::ViRegs update_screen_regs;

        ViState* get_next_state() {
            return &states[cur_state ^ 1];
        }
        ViState* get_cur_state() {
            return &states[cur_state];
        }
        void update_vi() {
            ViState* next_state = get_next_state();
            const OSViMode* next_mode = next_state->mode;
            const OSViCommonRegs* common_regs = &next_mode->comRegs;
            const OSViFieldRegs* field_regs = &next_mode->fldRegs[field];
            PTR(void) framebuffer = osVirtualToPhysical(next_state->framebuffer);
            PTR(void) origin = framebuffer + field_regs->origin;

            // Process the VI state flags.
            uint32_t hStart = common_regs->hStart;
            if (next_state->state & VI_STATE_BLACK) {
                hStart = 0;
            }

            uint32_t yScale = field_regs->yScale;
            if (next_state->state & VI_STATE_REPEATLINE) {
                yScale = 0;
                origin = framebuffer;
            }

            // TODO implement osViFade

            // Update VI registers.
            regs.VI_ORIGIN_REG = origin;
            regs.VI_WIDTH_REG = common_regs->width;
            regs.VI_TIMING_REG = common_regs->burst;
            regs.VI_V_SYNC_REG = common_regs->vSync;
            regs.VI_H_SYNC_REG = common_regs->hSync;
            regs.VI_LEAP_REG = common_regs->leap;
            regs.VI_H_START_REG = hStart;
            regs.VI_V_START_REG = field_regs->vStart; // TODO implement osViExtendVStart
            regs.VI_V_BURST_REG = field_regs->vBurst;
            regs.VI_INTR_REG = field_regs->vIntr;
            regs.VI_X_SCALE_REG = common_regs->xScale; // TODO implement osViSetXScale
            regs.VI_Y_SCALE_REG = yScale; // TODO implement osViSetYScale
            regs.VI_STATUS_REG = next_state->control;
            
            // Swap VI states.
            cur_state ^= 1;
            *get_next_state() = *get_cur_state();
        }
    } vi;
    struct {
        std::thread gfx_thread;
        std::thread task_thread;
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } sp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } dp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } ai;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } si;
    // The same message queue may be used for multiple events, so share a mutex for all of them
    std::mutex message_mutex;
    uint8_t* rdram;
    moodycamel::BlockingConcurrentQueue<Action> action_queue{};
    moodycamel::BlockingConcurrentQueue<OSTask*> sp_task_queue{};
    moodycamel::ConcurrentQueue<OSThread*> deleted_threads{};
} events_context{};

ultramodern::renderer::ViRegs* ultramodern::renderer::get_vi_regs() {
    return &events_context.vi.update_screen_regs;
}

#ifdef __ANDROID__
static constexpr size_t kAndroidViSnapshotCount = 4;
static std::mutex android_vi_snapshot_mutex;
static std::array<ultramodern::renderer::VIBufferSnapshot, kAndroidViSnapshotCount> android_vi_snapshots{};
static uint64_t android_vi_snapshot_sequence = 0;
static std::atomic_uint32_t screen_update_order_log_count = 0;

static bool should_queue_android_post_update_screen(const ViState &next_state, int field, uint32_t &origin_out, uint32_t &width_out) {
    origin_out = 0;
    width_out = 0;
    if (!ultramodern::is_game_started() || (next_state.mode == nullptr) || (next_state.state & VI_STATE_BLACK)) {
        return false;
    }

    const OSViCommonRegs *common_regs = &next_state.mode->comRegs;
    const OSViFieldRegs *field_regs = &next_state.mode->fldRegs[field];
    const uint32_t framebuffer = osVirtualToPhysical(next_state.framebuffer);
    uint32_t origin = framebuffer + field_regs->origin;
    if (next_state.state & VI_STATE_REPEATLINE) {
        origin = framebuffer;
    }

    origin_out = origin;
    width_out = common_regs->width;
    return (width_out != 320U) && (origin_out != 0U) && (origin_out < 0x00600000U);
}

static void store_android_vi_snapshot(uint32_t address, uint32_t width, uint32_t height, uint8_t siz, uint32_t origin_offset) {
    constexpr uint32_t kAndroidSnapshotRdramSize = 0x00800000U;
    if ((events_context.rdram == nullptr) || (address >= kAndroidSnapshotRdramSize) || (width == 0) || (height == 0) || (siz < 2U)) {
        return;
    }

    const uint32_t bytes_per_pixel = 1U << (siz - 1U);
    const uint64_t capture_bytes_u64 = uint64_t(origin_offset) + (uint64_t(width) * uint64_t(height) * uint64_t(bytes_per_pixel));
    if (capture_bytes_u64 == 0) {
        return;
    }

    const uint32_t capture_bytes = uint32_t(std::min<uint64_t>(capture_bytes_u64, uint64_t(kAndroidSnapshotRdramSize - address)));
    if (capture_bytes == 0) {
        return;
    }

    std::scoped_lock lock(android_vi_snapshot_mutex);
    ultramodern::renderer::VIBufferSnapshot *slot = nullptr;
    for (auto &candidate : android_vi_snapshots) {
        if (candidate.address == address) {
            slot = &candidate;
            break;
        }
    }

    if (slot == nullptr) {
        slot = &android_vi_snapshots[0];
        for (auto &candidate : android_vi_snapshots) {
            if (candidate.sequence < slot->sequence) {
                slot = &candidate;
            }
        }
    }

    slot->address = address;
    slot->width = width;
    slot->height = height;
    slot->siz = siz;
    slot->sequence = ++android_vi_snapshot_sequence;
    slot->bytes.resize(capture_bytes);
    std::memcpy(slot->bytes.data(), events_context.rdram + address, capture_bytes);
}
#endif

bool ultramodern::renderer::copy_vi_buffer_snapshot(uint32_t address, ultramodern::renderer::VIBufferSnapshot &out) {
#ifdef __ANDROID__
    std::scoped_lock lock(android_vi_snapshot_mutex);
    const ultramodern::renderer::VIBufferSnapshot *best = nullptr;
    for (const auto &candidate : android_vi_snapshots) {
        if ((candidate.address == address) && !candidate.bytes.empty()) {
            if ((best == nullptr) || (candidate.sequence > best->sequence)) {
                best = &candidate;
            }
        }
    }

    if (best != nullptr) {
        out = *best;
        return true;
    }
#else
    (void)address;
    (void)out;
#endif

    return false;
}

extern "C" void osSetEventMesg(RDRAM_ARG OSEvent event_id, PTR(OSMesgQueue) mq_, OSMesg msg) {
    std::lock_guard lock{ events_context.message_mutex };

    switch (event_id) {
        case OS_EVENT_SP:
            events_context.sp.msg = msg;
            events_context.sp.mq = mq_;
            break;
        case OS_EVENT_DP:
            events_context.dp.msg = msg;
            events_context.dp.mq = mq_;
            break;
        case OS_EVENT_AI:
            events_context.ai.msg = msg;
            events_context.ai.mq = mq_;
            break;
        case OS_EVENT_SI:
            events_context.si.msg = msg;
            events_context.si.mq = mq_;
    }
}

extern "C" void osViSetEvent(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, u32 retrace_count) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mq = mq_;
    next_state->msg = msg;
    next_state->retrace_count = retrace_count;
}

uint64_t total_vis = 0;


extern std::atomic_bool exited;
extern moodycamel::LightweightSemaphore graphics_shutdown_ready;

void set_dummy_vi(bool odd);

void vi_thread_func() {
    ultramodern::set_native_thread_name("VI Thread");
    // This thread should be prioritized over every other thread in the application, as it's what allows
    // the game to generate new audio and gfx lists.
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Critical);
    using namespace std::chrono_literals;

    int remaining_retraces = 1;

    while (!exited) {
        // Determine the next VI time (more accurate than adding 16ms each VI interrupt)
        auto next = ultramodern::get_start() + (total_vis * 1000000us) / (60 * ultramodern::get_speed_multiplier());
        //if (next > std::chrono::high_resolution_clock::now()) {
        //    printf("Sleeping for %" PRIu64 " us to get from %" PRIu64 " us to %" PRIu64 " us \n",
        //        (next - std::chrono::high_resolution_clock::now()) / 1us,
        //        (std::chrono::high_resolution_clock::now() - events_context.start) / 1us,
        //        (next - events_context.start) / 1us);
        //} else {
        //    printf("No need to sleep\n");
        //}
        // Detect if there's more than a second to wait and wait a fixed amount instead for the next VI if so, as that usually means the system clock went back in time.
        if (std::chrono::floor<std::chrono::seconds>(next - std::chrono::high_resolution_clock::now()) > 1s) {
            // printf("Skipping the next VI wait\n");
            next = std::chrono::high_resolution_clock::now();
        }
        ultramodern::sleep_until(next);
        auto time_now = ultramodern::time_since_start();
        // Calculate how many VIs have passed
        uint64_t new_total_vis = (time_now * (60 * ultramodern::get_speed_multiplier()) / 1000ms) + 1;
        if (new_total_vis > total_vis + 1) {
            //printf("Skipped % " PRId64 " frames in VI interupt thread!\n", new_total_vis - total_vis - 1);
        }
        total_vis = new_total_vis;

        // If the game hasn't started yet, set a dummy VI mode and origin.
        if (!ultramodern::is_game_started()) {
            static bool odd = false;
            set_dummy_vi(odd);
            odd = !odd;

            events_context.action_queue.enqueue(DummyWorkloadAction{events_context.vi.get_next_state()->framebuffer});
        }

        const ultramodern::renderer::ViRegs previous_vi_regs = events_context.vi.regs;
#if defined(__ANDROID__)
        uint32_t next_vi_origin = 0;
        uint32_t next_vi_width = 0;
        const bool queue_post_update_screen = should_queue_android_post_update_screen(*events_context.vi.get_next_state(), events_context.vi.field, next_vi_origin, next_vi_width);
        if (!queue_post_update_screen) {
#endif
            // Queue a screen update for the graphics thread with the current VI register state.
            // Doing this before the VI update is equivalent to updating the screen after the previous frame's scanout finished.
            events_context.action_queue.enqueue(ScreenUpdateAction{ previous_vi_regs });
#if defined(__ANDROID__)
        }
#endif

        // Update VI registers and swap VI modes.
        events_context.vi.update_vi();

#if defined(__ANDROID__)
        if (queue_post_update_screen) {
            events_context.action_queue.enqueue(ScreenUpdateAction{ events_context.vi.regs });
            if (screen_update_order_log_count.fetch_add(1) < 32U) {
                ULTRAMODERN_ANDROID_RENDER_LOG("ScreenUpdate queued post-VI prevOrigin=0x%08" PRIX32 " prevWidth=%" PRIu32 " nextOrigin=0x%08" PRIX32 " nextWidth=%" PRIu32,
                    previous_vi_regs.VI_ORIGIN_REG, previous_vi_regs.VI_WIDTH_REG, next_vi_origin, next_vi_width);
            }
        }
#endif

        // If the game has started, handle sending VI and AI events.
        if (ultramodern::is_game_started()) {
            remaining_retraces--;
            
            std::lock_guard lock{ events_context.message_mutex };
            ViState* cur_state = events_context.vi.get_cur_state();
            if (remaining_retraces == 0) {
                if (cur_state->mq != NULLPTR) {
                    // Send a message to the VI queue, and do not set it to be requeued if the queue was full.
                    // The worst case scenario is that the game misses a VI message and has to wait a little longer for the next. 
                    ultramodern::enqueue_external_message(cur_state->mq, cur_state->msg, false, false);
                }
                remaining_retraces = cur_state->retrace_count;
            }
            if (events_context.ai.mq != NULLPTR) {
                // Send a message to the VI queue, and do not set it to be requeued if the queue was full for the same reason as the VI message above.
                ultramodern::enqueue_external_message(events_context.ai.mq, events_context.ai.msg, false, false);
            }
        }

        if (events_callbacks.vi_callback != nullptr) {
            events_callbacks.vi_callback();
        }
    }
}

void sp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    ultramodern::enqueue_external_message(events_context.sp.mq, events_context.sp.msg, false, true);
}

void dp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    ultramodern::enqueue_external_message(events_context.dp.mq, events_context.dp.msg, false, true);
}

void task_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready) {
    ultramodern::set_native_thread_name("SP Task Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (true) {
        // Wait until an RSP task has been sent
        OSTask* task;
        events_context.sp_task_queue.wait_dequeue(task);

        if (task == nullptr) {
            return;
        }

        if (!ultramodern::rsp::run_task(PASS_RDRAM task)) {
            fprintf(stderr, "Failed to execute task type: %" PRIu32 "\n", task->t.type);
            ULTRAMODERN_QUICK_EXIT();
        }

        // Tell the game that the RSP has completed
        sp_complete();
    }
}

std::atomic_uint32_t display_refresh_rate = 60;
std::atomic<float> resolution_scale = 1.0f;

#ifdef __ANDROID__
static std::atomic_uint32_t sp_task_log_count = 0;
static std::atomic_uint32_t screen_update_log_count = 0;
static std::atomic_uint32_t vi_swap_log_count = 0;
static std::atomic_uint32_t vi_black_log_count = 0;

static bool should_log_android_render_event(std::atomic_uint32_t& counter) {
    uint32_t count = counter.fetch_add(1);
    return (count < 48) || ((count < 240) && ((count % 30U) == 0U));
}

static constexpr uint32_t kBanjoRdramBase = 0x80000000U;
static constexpr uint32_t kBanjoRdramSize = 0x00800000U;
static constexpr uint32_t kBanjoMainLoopStateAddr = 0x8027A130U;
static constexpr uint32_t kBanjoFramebufferWidthAddr = 0x80276588U;
static constexpr uint32_t kBanjoFramebufferHeightAddr = 0x8027658CU;
static constexpr uint32_t kBanjoBootMapAddr = 0x8027BEE8U;
static constexpr uint32_t kBanjoGameStateAddr = 0x8037E8E0U;
static constexpr uint32_t kBanjoTransitionStateAddr = 0x80382430U;
static constexpr uint32_t kBanjoLevelStateAddr = 0x80383300U;
static constexpr uint32_t kBanjoMapStateAddr = 0x803835D0U;

struct BanjoBootState {
    uint32_t main_loop_state = 0;
    uint32_t framebuffer_width = 0;
    uint32_t framebuffer_height = 0;
    uint32_t boot_map = 0;
    uint8_t current_level = 0;
    uint32_t current_map_state = 0;
    uint32_t current_map = 0;
    uint32_t current_exit = 0;
    uint32_t game_loop_counter = 0;
    uint32_t game_mode = 0;
    uint32_t freeze_scene = 0;
    uint8_t transition = 0;
    uint8_t map = 0;
    uint8_t exit = 0;
    uint8_t reset_on_load = 0;
    uint8_t unk18 = 0;
    uint8_t unk19 = 0;
    uint8_t pending_mode = 0;
    uint8_t pending_mode_arg = 0;
    uint8_t unk1c = 0;
    uint32_t transition_counter = 0;
    uint8_t transition_state = 0;
    float transition_timer = 0.0f;
};

struct AndroidFramebufferSample {
    uint32_t mean_byte = 0;
    uint32_t nonzero_samples = 0;
    uint32_t total_samples = 0;
};

static bool read_android_rdram_bytes(uint32_t address, void* out, size_t size) {
    if ((events_context.rdram == nullptr) || (address < kBanjoRdramBase)) {
        return false;
    }

    uint32_t offset = address - kBanjoRdramBase;
    if (offset > (kBanjoRdramSize - size)) {
        return false;
    }

    std::memcpy(out, events_context.rdram + offset, size);
    return true;
}

static bool read_android_rdram_u32(uint32_t address, uint32_t& out) {
    return read_android_rdram_bytes(address, &out, sizeof(out));
}

static bool read_android_rdram_u8(uint32_t address, uint8_t& out) {
    return read_android_rdram_bytes(address, &out, sizeof(out));
}

static bool read_android_rdram_f32(uint32_t address, float& out) {
    return read_android_rdram_bytes(address, &out, sizeof(out));
}

static bool read_banjo_boot_state(BanjoBootState& state) {
    return read_android_rdram_u32(kBanjoMainLoopStateAddr, state.main_loop_state)
        && read_android_rdram_u32(kBanjoFramebufferWidthAddr, state.framebuffer_width)
        && read_android_rdram_u32(kBanjoFramebufferHeightAddr, state.framebuffer_height)
        && read_android_rdram_u32(kBanjoBootMapAddr, state.boot_map)
        && read_android_rdram_u8(kBanjoLevelStateAddr + 0x01U, state.current_level)
        && read_android_rdram_u32(kBanjoMapStateAddr + 0x00U, state.current_map_state)
        && read_android_rdram_u32(kBanjoMapStateAddr + 0x04U, state.current_map)
        && read_android_rdram_u32(kBanjoMapStateAddr + 0x08U, state.current_exit)
        && read_android_rdram_u32(kBanjoGameStateAddr + 0x00U, state.game_loop_counter)
        && read_android_rdram_u32(kBanjoGameStateAddr + 0x04U, state.game_mode)
        && read_android_rdram_u32(kBanjoGameStateAddr + 0x0CU, state.freeze_scene)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x14U, state.transition)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x15U, state.map)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x16U, state.exit)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x17U, state.reset_on_load)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x18U, state.unk18)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x19U, state.unk19)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x1AU, state.pending_mode)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x1BU, state.pending_mode_arg)
        && read_android_rdram_u8(kBanjoGameStateAddr + 0x1CU, state.unk1c)
        && read_android_rdram_u32(kBanjoTransitionStateAddr + 0x00U, state.transition_counter)
        && read_android_rdram_u8(kBanjoTransitionStateAddr + 0x08U, state.transition_state)
        && read_android_rdram_f32(kBanjoTransitionStateAddr + 0x14U, state.transition_timer);
}

static bool transition_blocks_render(const BanjoBootState& state) {
    return (state.transition_state == 3U)
        || (state.transition_state == 5U)
        || (state.transition_state == 8U)
        || (((state.transition_state == 1U) || (state.transition_state == 6U)) && (state.transition_counter < 2U));
}

static AndroidFramebufferSample sample_android_framebuffer_bytes(uint32_t phys, uint32_t frame_bytes) {
    AndroidFramebufferSample sample{};
    if ((events_context.rdram == nullptr) || (phys >= kBanjoRdramSize)) {
        return sample;
    }

    frame_bytes = std::min(frame_bytes, kBanjoRdramSize - phys);
    if (frame_bytes == 0) {
        return sample;
    }

    uint32_t sample_count = std::min<uint32_t>(256U, frame_bytes);
    uint32_t sample_step = std::max(frame_bytes / sample_count, 1U);
    uint64_t byte_sum = 0;
    for (uint32_t offset = 0; offset < frame_bytes; offset += sample_step) {
        uint8_t value = events_context.rdram[phys + offset];
        byte_sum += value;
        sample.nonzero_samples += (value != 0);
        sample.total_samples++;
    }

    if (sample.total_samples > 0) {
        sample.mean_byte = uint32_t(byte_sum / sample.total_samples);
    }

    return sample;
}
#endif

uint32_t ultramodern::get_target_framerate(uint32_t original) {
    auto config = ultramodern::renderer::get_graphics_config();

    switch (config.rr_option) {
        case ultramodern::renderer::RefreshRate::Original:
        default:
            return original;
        case ultramodern::renderer::RefreshRate::Manual:
            return config.rr_manual_value;
        case ultramodern::renderer::RefreshRate::Display:
            return display_refresh_rate.load();
    }
}

uint32_t ultramodern::get_display_refresh_rate() {
    return display_refresh_rate.load();
}

float ultramodern::get_resolution_scale() {
    return resolution_scale.load();
}

void ultramodern::trigger_config_action() {
    events_context.action_queue.enqueue(UpdateConfigAction{});
}

std::atomic<ultramodern::renderer::SetupResult> renderer_setup_result = ultramodern::renderer::SetupResult::Success;
std::atomic<ultramodern::renderer::GraphicsApi> renderer_chosen_api = ultramodern::renderer::GraphicsApi::Auto;

void gfx_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready, ultramodern::renderer::WindowHandle window_handle) {
    bool enabled_instant_present = false;
    using namespace std::chrono_literals;

    ultramodern::set_native_thread_name("Gfx Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    auto old_config = ultramodern::renderer::get_graphics_config();

    auto create_config = ultramodern::renderer::get_graphics_config();
    auto renderer_context = ultramodern::renderer::create_render_context(rdram, window_handle, create_config.developer_mode);

    renderer_chosen_api.store(renderer_context->get_chosen_api());
    if (!renderer_context->valid()) {
        renderer_setup_result.store(renderer_context->get_setup_result());
        // Notify the caller thread that this thread is ready.
        thread_ready->signal();
        return;
    }

    if (events_callbacks.gfx_init_callback != nullptr) {
        events_callbacks.gfx_init_callback();
    }

    ultramodern::rsp::init();

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    std::optional<Action> deferred_action;

    while (!exited) {
        // Try to pull an action from the queue
        Action action;
        bool has_action = false;
        if (deferred_action.has_value()) {
            action = std::move(*deferred_action);
            deferred_action.reset();
            has_action = true;
        }
        else {
            has_action = events_context.action_queue.wait_dequeue_timed(action, 1ms);
        }

        if (has_action) {
            // Determine the action type and act on it
            if (const auto* task_action = std::get_if<SpTaskAction>(&action)) {
                // Tell the game that the RSP completed instantly. This will allow it to queue other task types, but it won't
                // start another graphics task until the RDP is also complete. Games usually preserve the RSP inputs until the RDP
                // is finished as well, so sending this early shouldn't be an issue in most cases.
                // If this causes issues then the logic can be replaced with responding to yield requests.
                sp_complete();
                ultramodern::measure_input_latency();

                PTR(u64) displaylist = task_action->task.t.data_ptr;
                ultramodern::extensions::on_displaylist_submitted(displaylist);

#ifdef __ANDROID__
                const bool log_sp_task = should_log_android_render_event(sp_task_log_count);
                if (log_sp_task) {
                    ULTRAMODERN_ANDROID_RENDER_LOG("SpTask begin dl=0x%08" PRIX32 " ucode=0x%08" PRIX32,
                        uint32_t(task_action->task.t.data_ptr), uint32_t(task_action->task.t.ucode));
                }
#endif

                [[maybe_unused]] auto renderer_start = std::chrono::high_resolution_clock::now();
                renderer_context->send_dl(&task_action->task);
                [[maybe_unused]] auto renderer_end = std::chrono::high_resolution_clock::now();

#ifdef __ANDROID__
                if (log_sp_task) {
                    ULTRAMODERN_ANDROID_RENDER_LOG("SpTask end dl=0x%08" PRIX32, uint32_t(task_action->task.t.data_ptr));
                }
#endif

                dp_complete();
                // TODO hook the parsed event up to the actual parsing point when a callback is added to RT64.
                ultramodern::extensions::on_displaylist_parsed(displaylist);
                ultramodern::extensions::on_displaylist_completed(displaylist);
                // printf("Renderer ProcessDList time: %d us\n", static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count()));
            }
            else if (const auto* screen_update_action = std::get_if<ScreenUpdateAction>(&action)) {
                ScreenUpdateAction coalesced_screen_update = *screen_update_action;
#ifdef __ANDROID__
                Action queued_action;
                while (events_context.action_queue.try_dequeue(queued_action)) {
                    if (const auto* next_screen_update = std::get_if<ScreenUpdateAction>(&queued_action)) {
                        coalesced_screen_update = *next_screen_update;
                    }
                    else {
                        deferred_action = std::move(queued_action);
                        break;
                    }
                }
#endif
#ifdef __ANDROID__
                const bool log_screen_update = should_log_android_render_event(screen_update_log_count);
                if (log_screen_update) {
                    uint32_t origin = coalesced_screen_update.regs.VI_ORIGIN_REG;
                    uint32_t width = coalesced_screen_update.regs.VI_WIDTH_REG;
                    uint32_t type = coalesced_screen_update.regs.VI_STATUS_REG & 0x3U;
                    uint32_t bytes_per_pixel = (type == 3U) ? 4U : 2U;
                    AndroidFramebufferSample sample = sample_android_framebuffer_bytes(origin, width * 240U * bytes_per_pixel);
                    ULTRAMODERN_ANDROID_RENDER_LOG("ScreenUpdate begin origin=0x%08" PRIX32 " width=%" PRIu32 " status=0x%08" PRIX32,
                        origin, width, coalesced_screen_update.regs.VI_STATUS_REG);
                    ULTRAMODERN_ANDROID_RENDER_LOG("ScreenUpdate sample origin=0x%08" PRIX32 " meanByte=%" PRIu32 " nonzero=%" PRIu32 "/%" PRIu32,
                        origin, sample.mean_byte, sample.nonzero_samples, sample.total_samples);
                }
#endif
                events_context.vi.update_screen_regs = coalesced_screen_update.regs;
                renderer_context->update_screen();
                display_refresh_rate = renderer_context->get_display_framerate();
                resolution_scale = renderer_context->get_resolution_scale();
#ifdef __ANDROID__
                if (log_screen_update) {
                    ULTRAMODERN_ANDROID_RENDER_LOG("ScreenUpdate end rate=%" PRIu32 " scale=%.2f",
                        display_refresh_rate.load(), resolution_scale.load());
                }
#endif
            }
            else if (const auto* config_action = std::get_if<UpdateConfigAction>(&action)) {
                (void)config_action;
                auto new_config = ultramodern::renderer::get_graphics_config();
                if (renderer_context->update_config(old_config, new_config)) {
                    old_config = new_config;
                }
            }
            else if (const auto* dummy_workload_action = std::get_if<DummyWorkloadAction>(&action)) {
                renderer_context->send_dummy_workload(dummy_workload_action->fb_address);
            }
        }
    }

    graphics_shutdown_ready.wait();
    renderer_context->shutdown();
}

#define VI_CTRL_TYPE_16             0x00002
#define VI_CTRL_TYPE_32             0x00003
#define VI_CTRL_GAMMA_DITHER_ON     0x00004
#define VI_CTRL_GAMMA_ON            0x00008
#define VI_CTRL_DIVOT_ON            0x00010
#define VI_CTRL_SERRATE_ON          0x00040
#define VI_CTRL_ANTIALIAS_MASK      0x00300
#define VI_CTRL_ANTIALIAS_MODE_1    0x00100
#define VI_CTRL_ANTIALIAS_MODE_2    0x00200
#define VI_CTRL_ANTIALIAS_MODE_3    0x00300
#define VI_CTRL_PIXEL_ADV_MASK      0x01000
#define VI_CTRL_PIXEL_ADV_1         0x01000
#define VI_CTRL_PIXEL_ADV_2         0x02000
#define VI_CTRL_PIXEL_ADV_3         0x03000
#define VI_CTRL_DITHER_FILTER_ON    0x10000

static const OSViMode dummy_mode = []() {
    OSViMode ret{};

    ret.type = 2;
    ret.comRegs.ctrl = VI_CTRL_TYPE_16 | VI_CTRL_GAMMA_DITHER_ON | VI_CTRL_GAMMA_ON | VI_CTRL_DIVOT_ON | VI_CTRL_ANTIALIAS_MODE_1 | VI_CTRL_PIXEL_ADV_3;
    ret.comRegs.width = 0x140;
    ret.comRegs.burst = 0x03E52239;
    ret.comRegs.vSync = 0x20D;
    ret.comRegs.hSync = 0xC15;
    ret.comRegs.leap = 0x0C150C15;
    ret.comRegs.hStart = 0x006C02EC;
    ret.comRegs.xScale = 0x200;
    ret.comRegs.vCurrent = 0x0;

    for (int field = 0; field < 2; field++) {
        ret.fldRegs[field].origin = 0x280;
        ret.fldRegs[field].yScale = 0x400;
        ret.fldRegs[field].vStart = 0x2501FF;
        ret.fldRegs[field].vBurst = 0xE0204;
        ret.fldRegs[field].vIntr = 0x2;
    }

    return ret;
}();

void set_dummy_vi(bool odd) {
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = &dummy_mode;
    next_state->control = next_state->mode->comRegs.ctrl;
    // Set up a dummy framebuffer.
    next_state->framebuffer = 0x80700000;
    if (odd) {
        next_state->framebuffer += 0x25800;
    }
}

extern "C" void osViSwapBuffer(RDRAM_ARG PTR(void) frameBufPtr) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    next_state->framebuffer = frameBufPtr;
#ifdef __ANDROID__
    uint32_t phys = osVirtualToPhysical(frameBufPtr);
    uint32_t width = (next_state->mode != nullptr) ? next_state->mode->comRegs.width : 320U;
    uint32_t type = next_state->control & 0x3U;
    uint32_t bytes_per_pixel = (type == 3U) ? 4U : 2U;
    uint32_t frame_bytes = width * 240U * bytes_per_pixel;
    AndroidFramebufferSample sample = sample_android_framebuffer_bytes(phys, frame_bytes);

    BanjoBootState boot_state{};
    const bool has_boot_state = read_banjo_boot_state(boot_state);
    const uint32_t snapshot_height = has_boot_state ? std::max<uint32_t>(boot_state.framebuffer_height, 1U) : 240U;
    uint32_t origin_offset = 0;
    if (next_state->mode != nullptr) {
        origin_offset = next_state->mode->fldRegs[events_context.vi.field].origin;
        if (next_state->state & VI_STATE_REPEATLINE) {
            origin_offset = 0;
        }
    }

    const uint8_t snapshot_siz = (bytes_per_pixel == 4U) ? 3U : 2U;
    store_android_vi_snapshot(phys, width, snapshot_height, snapshot_siz, origin_offset);

    if (should_log_android_render_event(vi_swap_log_count)) {
        if (has_boot_state) {
            ULTRAMODERN_ANDROID_RENDER_LOG(
                "osViSwapBuffer framebuffer=0x%08" PRIX32 " phys=0x%08" PRIX32 " width=%" PRIu32 " bpp=%" PRIu32
                " meanByte=%" PRIu32 " nonzero=%" PRIu32 "/%" PRIu32
                " state=%" PRIu32 " gameMode=%" PRIu32 " framebuf=%" PRIu32 "x%" PRIu32
                " bootMap=%" PRIu32 " level=%u mapState=%" PRIu32 " currentMap=%" PRIu32 " currentExit=%" PRIu32 " loop=%" PRIu32
                " gameTransition=%u map=%u exit=%u reset=%u pendingMode=%u/%u freeze=%" PRIu32
                " gcState=%u gcCounter=%" PRIu32 " gcTimer=%.3f gcBlocks=%u",
                uint32_t(frameBufPtr), phys, width, bytes_per_pixel, sample.mean_byte, sample.nonzero_samples, sample.total_samples,
                boot_state.main_loop_state, boot_state.game_mode, boot_state.framebuffer_width, boot_state.framebuffer_height,
                boot_state.boot_map, uint32_t(boot_state.current_level), boot_state.current_map_state, boot_state.current_map, boot_state.current_exit, boot_state.game_loop_counter,
                uint32_t(boot_state.transition), uint32_t(boot_state.map), uint32_t(boot_state.exit), uint32_t(boot_state.reset_on_load),
                uint32_t(boot_state.pending_mode), uint32_t(boot_state.pending_mode_arg), boot_state.freeze_scene,
                uint32_t(boot_state.transition_state), boot_state.transition_counter, boot_state.transition_timer,
                uint32_t(transition_blocks_render(boot_state)));
        }
        else {
            ULTRAMODERN_ANDROID_RENDER_LOG(
                "osViSwapBuffer framebuffer=0x%08" PRIX32 " phys=0x%08" PRIX32 " width=%" PRIu32 " bpp=%" PRIu32 " meanByte=%" PRIu32 " nonzero=%" PRIu32 "/%" PRIu32,
                uint32_t(frameBufPtr), phys, width, bytes_per_pixel, sample.mean_byte, sample.nonzero_samples, sample.total_samples);
        }
    }
#endif
}

extern "C" void osViSetMode(RDRAM_ARG PTR(OSViMode) mode_) {
    std::lock_guard lock{ events_context.message_mutex };
    OSViMode* mode = TO_PTR(OSViMode, mode_);
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = mode;
    next_state->control = next_state->mode->comRegs.ctrl;
}

#define OS_VI_GAMMA_ON          0x0001
#define OS_VI_GAMMA_OFF         0x0002
#define OS_VI_GAMMA_DITHER_ON   0x0004
#define OS_VI_GAMMA_DITHER_OFF  0x0008
#define OS_VI_DIVOT_ON          0x0010
#define OS_VI_DIVOT_OFF         0x0020
#define OS_VI_DITHER_FILTER_ON  0x0040
#define OS_VI_DITHER_FILTER_OFF 0x0080

extern "C" void osViSetSpecialFeatures(uint32_t func) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* control_out = &next_state->control;
    if ((func & OS_VI_GAMMA_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_DIVOT_ON) != 0) {
        *control_out |= VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DIVOT_OFF) != 0) {
        *control_out &= ~VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DITHER_FILTER_ON) != 0) {
        *control_out |= VI_CTRL_DITHER_FILTER_ON;
        *control_out &= ~VI_CTRL_ANTIALIAS_MASK;
    }

    if ((func & OS_VI_DITHER_FILTER_OFF) != 0) {
        *control_out &= ~VI_CTRL_DITHER_FILTER_ON;
        *control_out |= next_state->mode->comRegs.ctrl & VI_CTRL_ANTIALIAS_MASK;
    }
}

extern "C" void osViBlack(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_BLACK;
    } else {
        *state_out &= ~VI_STATE_BLACK;
    }
#ifdef __ANDROID__
    if (should_log_android_render_event(vi_black_log_count)) {
        ULTRAMODERN_ANDROID_RENDER_LOG("osViBlack active=%u state=0x%08" PRIX32, uint32_t(active), *state_out);
    }
#endif
}

extern "C" void osViRepeatLine(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_REPEATLINE;
    } else {
        *state_out &= ~VI_STATE_REPEATLINE;
    }
}

extern "C" void osViSetXScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" void osViSetYScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" PTR(void) osViGetNextFramebuffer() {
    return events_context.vi.get_next_state()->framebuffer;
}

extern "C" PTR(void) osViGetCurrentFramebuffer() {
    return events_context.vi.get_cur_state()->framebuffer;
}

void ultramodern::submit_rsp_task(RDRAM_ARG PTR(OSTask) task_) {
    OSTask* task = TO_PTR(OSTask, task_);

    // Send gfx tasks to the graphics action queue
    if (task->t.type == M_GFXTASK) {
        events_context.action_queue.enqueue(SpTaskAction{ *task });
    }
    // Set all other tasks as the RSP task
    else {
        events_context.sp_task_queue.enqueue(task);
    }
}

void ultramodern::send_si_message() {
    ultramodern::enqueue_external_message(events_context.si.mq, events_context.si.msg, false, true);
}

void ultramodern::init_events(RDRAM_ARG ultramodern::renderer::WindowHandle window_handle) {
    moodycamel::LightweightSemaphore gfx_thread_ready;
    moodycamel::LightweightSemaphore task_thread_ready;
    events_context.rdram = rdram;
    events_context.sp.gfx_thread = std::thread{ gfx_thread_func, rdram, &gfx_thread_ready, window_handle };
    events_context.sp.task_thread = std::thread{ task_thread_func, rdram, &task_thread_ready };

    // Wait for the two sp threads to be ready before continuing to prevent the game from
    // running before we're able to handle RSP tasks.
    gfx_thread_ready.wait();
    task_thread_ready.wait();

    ultramodern::renderer::SetupResult setup_result = renderer_setup_result.load();
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        auto show_renderer_error = [](const std::string& msg) {
            std::string error_msg = "An error has been encountered on startup: " + msg;

            ultramodern::error_handling::message_box(error_msg.c_str());
        };

        const std::string driver_os_suffix = "\nPlease make sure your GPU drivers and your OS are up to date.";
        switch (setup_result) {
            case ultramodern::renderer::SetupResult::Success:
                break;
            case ultramodern::renderer::SetupResult::DynamicLibrariesNotFound:
                show_renderer_error("Failed to load dynamic libraries. Make sure the DLLs are next to the recomp executable.");
                break;
            case ultramodern::renderer::SetupResult::InvalidGraphicsAPI:
                show_renderer_error(ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + " is not supported on this platform. Please select a different graphics API.");
                break;
            case ultramodern::renderer::SetupResult::GraphicsAPINotFound:
                show_renderer_error("Unable to initialize " + ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + "." + driver_os_suffix);
                break;
            case ultramodern::renderer::SetupResult::GraphicsDeviceNotFound:
                show_renderer_error("Unable to find compatible graphics device." + driver_os_suffix);
                break;
        }
        throw std::runtime_error("Failed to initialize the renderer");
    }

    events_context.vi.thread = std::thread{ vi_thread_func };
}

void ultramodern::join_event_threads() {
    events_context.sp.gfx_thread.join();
    events_context.vi.thread.join();

    // Send a null RSP task to indicate that the RSP task thread should exit.
    events_context.sp_task_queue.enqueue(nullptr);
    events_context.sp.task_thread.join();
}
