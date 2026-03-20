#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "blockingconcurrentqueue.h"

#include "ultramodern/threads.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#if defined(BANJO_ENABLE_ANDROID_TRACE_LOGS)
#define BANJO_ANDROID_THREAD_LOG(...) __android_log_print(ANDROID_LOG_INFO, "BanjoThread", __VA_ARGS__)
#else
#define BANJO_ANDROID_THREAD_LOG(...) ((void)0)
#endif
#define BANJO_ANDROID_THREAD_INFO(...) __android_log_print(ANDROID_LOG_INFO, "BanjoThread", __VA_ARGS__)
#define BANJO_ANDROID_THREAD_WARN(...) __android_log_print(ANDROID_LOG_WARN, "BanjoThread", __VA_ARGS__)
#else
#define BANJO_ANDROID_THREAD_LOG(...) ((void)0)
#define BANJO_ANDROID_THREAD_INFO(...) ((void)0)
#define BANJO_ANDROID_THREAD_WARN(...) ((void)0)
#endif

// Native APIs only used to set thread names for easier debugging
#ifdef _WIN32
#include <Windows.h>
#endif

static ultramodern::threads::callbacks_t threads_callbacks;

void ultramodern::threads::set_callbacks(const callbacks_t& callbacks) {
    threads_callbacks = callbacks;
}

std::string ultramodern::threads::get_game_thread_name(const OSThread* t) {
    if (threads_callbacks.get_game_thread_name == nullptr) {
        return "Game Thread " + std::to_string(t->id);
    }
    return threads_callbacks.get_game_thread_name(t);
}

extern "C" void bootproc();

thread_local bool is_main_thread = false;
// Whether this thread is part of the game (i.e. the start thread or one spawned by osCreateThread)
thread_local bool is_game_thread = false;
thread_local PTR(OSThread) thread_self = NULLPTR;

void ultramodern::set_main_thread() {
    ::is_game_thread = true;
    is_main_thread = true;
}

bool ultramodern::is_game_thread() {
    return ::is_game_thread;
}

#if 0
int main(int argc, char** argv) {
    ultramodern::set_main_thread();

    bootproc();
}
#endif

#if 1
void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg);
#else
#define run_thread_function(func, sp, arg) func(arg)
#endif

#if defined(_WIN32)
void ultramodern::set_native_thread_name(const std::string& name) {
    std::wstring wname{name.begin(), name.end()};

    HRESULT r;
    r = SetThreadDescription(
        GetCurrentThread(),
        wname.c_str()
    );
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    int nPriority = THREAD_PRIORITY_NORMAL;

    // Convert ThreadPriority to Win32 priority
    switch (pri) {
        case ThreadPriority::Low:
            nPriority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case ThreadPriority::Normal:
            nPriority = THREAD_PRIORITY_NORMAL;
            break;
        case ThreadPriority::High:
            nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ThreadPriority::VeryHigh:
            nPriority = THREAD_PRIORITY_HIGHEST;
            break;
        case ThreadPriority::Critical:
            nPriority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        default:
            throw std::runtime_error("Invalid thread priority!");
            break;
    }
    // SetThreadPriority(GetCurrentThread(), nPriority);
}
#elif defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
    int get_current_thread_id() {
        return static_cast<int>(syscall(SYS_gettid));
    }

    int get_target_nice(ultramodern::ThreadPriority pri) {
        switch (pri) {
            case ultramodern::ThreadPriority::Low:
                return 10;
            case ultramodern::ThreadPriority::Normal:
                return 0;
            case ultramodern::ThreadPriority::High:
                return -4;
            case ultramodern::ThreadPriority::VeryHigh:
                return -8;
            case ultramodern::ThreadPriority::Critical:
                return -10;
            default:
                throw std::runtime_error("Invalid thread priority!");
        }
    }

    const char *get_thread_priority_name(ultramodern::ThreadPriority pri) {
        switch (pri) {
            case ultramodern::ThreadPriority::Low:
                return "low";
            case ultramodern::ThreadPriority::Normal:
                return "normal";
            case ultramodern::ThreadPriority::High:
                return "high";
            case ultramodern::ThreadPriority::VeryHigh:
                return "very_high";
            case ultramodern::ThreadPriority::Critical:
                return "critical";
            default:
                return "unknown";
        }
    }
}

void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Linux only accepts up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    prctl(PR_SET_NAME, name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    const int thread_id = get_current_thread_id();

    errno = 0;
    const int current_nice = getpriority(PRIO_PROCESS, thread_id);
    if ((current_nice == -1) && (errno != 0)) {
        BANJO_ANDROID_THREAD_WARN("getpriority failed tid=%d requested=%s error=%s",
            thread_id, get_thread_priority_name(pri), std::strerror(errno));
        return;
    }

    const int target_nice = get_target_nice(pri);
    int desired_nice = current_nice;
    switch (pri) {
        case ThreadPriority::Low:
        case ThreadPriority::Normal:
            break;
        case ThreadPriority::High:
        case ThreadPriority::VeryHigh:
        case ThreadPriority::Critical:
            desired_nice = std::min(current_nice, target_nice);
            break;
        default:
            throw std::runtime_error("Invalid thread priority!");
    }

    if (desired_nice == current_nice) {
        BANJO_ANDROID_THREAD_INFO("thread_priority kept tid=%d requested=%s nice=%d",
            thread_id, get_thread_priority_name(pri), current_nice);
        return;
    }

    if (setpriority(PRIO_PROCESS, thread_id, desired_nice) != 0) {
        BANJO_ANDROID_THREAD_WARN("setpriority failed tid=%d requested=%s from=%d to=%d error=%s",
            thread_id, get_thread_priority_name(pri), current_nice, desired_nice, std::strerror(errno));
        return;
    }

    errno = 0;
    int applied_nice = getpriority(PRIO_PROCESS, thread_id);
    if ((applied_nice == -1) && (errno != 0)) {
        applied_nice = desired_nice;
    }

    BANJO_ANDROID_THREAD_INFO("thread_priority set tid=%d requested=%s nice=%d->%d",
        thread_id, get_thread_priority_name(pri), current_nice, applied_nice);
}
#elif defined(__APPLE__)
void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Macs seem to only accept up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    pthread_setname_np(name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {}
#endif

void wait_for_resumed(RDRAM_ARG UltraThreadContext* thread_context) {
    thread_context->running.wait();
    // If this thread's context was replaced by another thread or deleted, destroy it again from its own context.
    // This will trigger thread cleanup instead.
    if (TO_PTR(OSThread, ultramodern::this_thread())->context != thread_context) {
        osDestroyThread(PASS_RDRAM NULLPTR);
    }
}

void resume_thread(OSThread* t) {
    debug_printf("[Thread] Resuming execution of thread %d\n", t->id);
    BANJO_ANDROID_THREAD_LOG("resume_thread id=%d state=%d", t->id, t->state);
    t->context->running.signal();
}

void run_next_thread(RDRAM_ARG1) {
    if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        throw std::runtime_error("No threads left to run!\n");
    }

    OSThread* to_run = TO_PTR(OSThread, ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue));
    debug_printf("[Scheduling] Resuming execution of thread %d\n", to_run->id);
    BANJO_ANDROID_THREAD_LOG("run_next_thread id=%d state=%d", to_run->id, to_run->state);
    to_run->context->running.signal();
}

void ultramodern::run_next_thread_and_wait(RDRAM_ARG1) {
    UltraThreadContext* cur_context = TO_PTR(OSThread, thread_self)->context;
    run_next_thread(PASS_RDRAM1);
    wait_for_resumed(PASS_RDRAM cur_context);
}

void ultramodern::resume_thread_and_wait(RDRAM_ARG OSThread *t) {
    UltraThreadContext* cur_context = TO_PTR(OSThread, thread_self)->context;
    resume_thread(t);
    wait_for_resumed(PASS_RDRAM cur_context);
}

static void _thread_func(RDRAM_ARG PTR(OSThread) self_, PTR(thread_func_t) entrypoint, PTR(void) arg, UltraThreadContext* thread_context) {
    OSThread *self = TO_PTR(OSThread, self_);
    debug_printf("[Thread] Thread created: %d\n", self->id);
    BANJO_ANDROID_THREAD_LOG("thread_created id=%d entry=0x%08" PRIX32 " arg=0x%08" PRIX32,
        self->id, uint32_t(entrypoint), uint32_t(arg));
    thread_self = self_;
    is_game_thread = true;

    // Set the thread name
    ultramodern::set_native_thread_name(ultramodern::threads::get_game_thread_name(self));
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);

    // Signal the initialized semaphore to indicate that this thread can be started.
    thread_context->initialized.signal();

    debug_printf("[Thread] Thread waiting to be started: %d\n", self->id);

    // Wait until the thread is marked as running.
    try {
        wait_for_resumed(PASS_RDRAM thread_context);
    } catch (ultramodern::thread_terminated& terminated) {
    }

    // Make sure the thread wasn't replaced or destroyed before it was started.
    if (self->context == thread_context) {
        debug_printf("[Thread] Thread started: %d\n", self->id);
        BANJO_ANDROID_THREAD_LOG("thread_started id=%d", self->id);
        try {
            // Run the thread's function with the provided argument.
            run_thread_function(PASS_RDRAM entrypoint, self->sp, arg);
        } catch (ultramodern::thread_terminated& terminated) {
            BANJO_ANDROID_THREAD_LOG("thread_terminated id=%d", self->id);
        }
    }
    else {
        debug_printf("[Thread] Thread destroyed before being started: %d\n", self->id);
        BANJO_ANDROID_THREAD_LOG("thread_destroyed_before_start id=%d", self->id);
    }

    // Check if the thread hasn't been destroyed or replaced. If so, then the thread terminated or destroyed itself,
    // so mark this thread as destroyed and run the next queued thread.
    if (self->context == thread_context) {
        self->context = nullptr;
        run_next_thread(PASS_RDRAM1);
    }

    // Dispose of this thread now that it's completed or terminated.
    ultramodern::cleanup_thread(thread_context);
    BANJO_ANDROID_THREAD_LOG("thread_cleanup id=%d", self->id);
}

extern "C" void osStartThread(RDRAM_ARG PTR(OSThread) t_) {
    OSThread* t = TO_PTR(OSThread, t_);
    debug_printf("[os] Start Thread %d\n", t->id);
    BANJO_ANDROID_THREAD_LOG("osStartThread id=%d caller_self=%d", t->id, thread_self != NULLPTR);

    // If this is a game thread, insert the new thread into the running queue and then check the running queue.
    if (thread_self) {
        ultramodern::schedule_running_thread(PASS_RDRAM t_);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
    // Otherwise, immediately start the thread and terminate this one.
    else {
        t->state = OSThreadState::QUEUED;
        resume_thread(t);
        //throw ultramodern::thread_terminated{};
    }
}

extern "C" void osCreateThread(RDRAM_ARG PTR(OSThread) t_, OSId id, PTR(thread_func_t) entrypoint, PTR(void) arg, PTR(void) sp, OSPri pri) {
    debug_printf("[os] Create Thread %d\n", id);
    BANJO_ANDROID_THREAD_LOG("osCreateThread id=%d entry=0x%08" PRIX32 " arg=0x%08" PRIX32 " pri=%d",
        id, uint32_t(entrypoint), uint32_t(arg), pri);
    OSThread *t = TO_PTR(OSThread, t_);
    
    t->next = NULLPTR;
    t->queue = NULLPTR;
    t->priority = pri;
    t->id = id;
    t->state = OSThreadState::STOPPED;
    t->sp = sp - 0x10; // Set up the first stack frame

    // Spawn a new thread, which will immediately pause itself and wait until it's been started.
    // Pass the context as an argument to the thread function to ensure that it can't get cleared before the thread captures its value.
    UltraThreadContext* context = new UltraThreadContext{};
    t->context = context;
    context->host_thread = std::thread{_thread_func, PASS_RDRAM t_, entrypoint, arg, t->context};

    // Wait until the thread is initialized to indicate that it's ready to be started.
    context->initialized.wait();
    debug_printf("[os] Thread %d is ready to be started\n", t->id);
}

extern "C" void osStopThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    // Check if the thread is stopping itself (arg is null or thread_self).
    if (t_ == thread_self) {
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
    }
    else {
        assert(false);
    }
}

extern "C" void osDestroyThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    // Check if the thread is destroying itself (arg is null or thread_self)
    if (t_ == thread_self) {
        throw ultramodern::thread_terminated{};
    }
    // Otherwise if the thread isn't stopped, remove it from its currrent queue., 
    if (t->state != OSThreadState::STOPPED) {
        ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
    }
    // Check if the thread has already been destroyed to prevent destroying it again.
    UltraThreadContext* cur_context = t->context;
    if (cur_context != nullptr) {
        // Mark the target thread as destroyed and resume it. When it starts it'll check this and terminate itself instead of resuming.
        t->context = nullptr;
        cur_context->running.signal();
    }
}

extern "C" void osSetThreadPri(RDRAM_ARG PTR(OSThread) t_, OSPri pri) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);

    if (t->priority != pri) {
        t->priority = pri;

        if (t_ != ultramodern::this_thread() && t->state != OSThreadState::STOPPED) {
            ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
            ultramodern::thread_queue_insert(PASS_RDRAM t->queue, t_);
        }

        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" OSPri osGetThreadPri(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->priority;
}

extern "C" OSId osGetThreadId(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->id;
}

PTR(OSThread) ultramodern::this_thread() {
    return thread_self;
}

static std::thread thread_cleaner_thread;
static moodycamel::BlockingConcurrentQueue<UltraThreadContext*> deleted_threads{};
extern std::atomic_bool exited;

void thread_cleaner_func() {
    using namespace std::chrono_literals;
    while (!exited) {
        UltraThreadContext* to_delete;
        if (deleted_threads.wait_dequeue_timed(to_delete, 10ms)) {
            debug_printf("[Cleanup] Deleting thread context %p\n", to_delete);

            to_delete->host_thread.join();
            delete to_delete;
        }
    }
}

void ultramodern::init_thread_cleanup() {
    thread_cleaner_thread = std::thread{thread_cleaner_func};
}

void ultramodern::cleanup_thread(UltraThreadContext *cur_context) {
    deleted_threads.enqueue(cur_context);
}

void ultramodern::join_thread_cleaner_thread() {
    thread_cleaner_thread.join();
}
