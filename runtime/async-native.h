// yel's async runtime natively: its reactor is libuv's event loop (runtime/libuv), as a component's
// is its host. Timers, and the requests the native WASI calls start (runtime/wasi-native.h: the file
// system's, on libuv's thread pool), finish there and wake the tasks waiting on them. A component's
// waits for its own host's streams and futures across WIT are stubs that stop the program: build
// such a program as a component (runtime/async-wasi.h).

#include <uv.h>

// requests in flight on the loop (each wakes its task when it finishes)
extern int64_t y_uv_pending;

// the timer the loop waits on until the first of the executor's timers is due
extern uv_timer_t y_uv_timer;
extern bool y_uv_timer_ready;

void y_uv_timer_fired(uv_timer_t *timer);

/** The reactor: no task ready, so the loop runs until a timer is due or a request finishes, and the
tasks those wake are woken. False when nothing will wake any task (every one waits for another, or
for nothing). */
bool yel_async_await(void);

typedef struct { y_async *root; uint32_t set; void (*finish)(y_async *root); int32_t returned; } y_hostroot;

Y_NORETURN void y_no_host(void);

y_hostroot *y_host_root(y_async *root);

void y_host_root_end(y_async *root);

void y_async_host_wait(uint32_t waitable);

bool y_async_host_done(uint32_t waitable);

uint32_t y_async_host_code(uint32_t waitable);

void y_async_host_forget(uint32_t waitable);

void y_async_host_drop(uint32_t waitable);

bool y_async_host_pending(y_async *root);

y_async *y_async_host_event(uint32_t event, uint32_t waitable, uint32_t code);

#define Y_BLOCKED 0xffffffffu

ystream *y_stream_from_host(uint32_t end, uint32_t (*read)(uint32_t, uint8_t *, size_t),
	void (*lift)(ystream *, void *, int32_t), void (*drop)(uint32_t), size_t abi_size, int64_t size, y_scan scan);

uint32_t y_stream_to_host(ystream *s, uint64_t (*new_pair)(void), uint32_t (*write)(uint32_t, const uint8_t *, size_t),
	void (*lower)(void *, void *, int32_t), void (*drop)(uint32_t), size_t abi_size);

/* natively no stream is the host's: nothing to let go */
#define yel_stream_release(T, S, s) ((void)(s))

y_async *yel_stream_fill(ystream *s);

