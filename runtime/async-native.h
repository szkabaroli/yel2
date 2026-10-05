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

/** The task being stepped, to be woken once fd can be read (events 1) or written (2) without
blocking; true when it waits so (it parks next), false for one that never waits (a regular file). */
bool yel_async_ready(int64_t fd, int64_t events);

/** fd about to be closed: its watch ended first, its waiters woken. */
yunit yel_async_forget(int64_t fd);

/** A blocking call begun on libuv's thread pool, its task woken when it is done: the request. */
int64_t yel_pool_pread(int64_t fd, int64_t at, int64_t count, int64_t offset);
int64_t yel_pool_pwrite(int64_t fd, int64_t at, int64_t count, int64_t offset);
int64_t yel_pool_openat(int64_t dir, int64_t path, int64_t options);
int64_t yel_pool_mkdirat(int64_t dir, int64_t path);
int64_t yel_pool_unlinkat(int64_t dir, int64_t path, int64_t flags);
int64_t yel_pool_renameat(int64_t from_dir, int64_t from, int64_t to_dir, int64_t to);
int64_t yel_pool_stat(int64_t fd, int64_t path, int64_t follow, int64_t out);
/** A name's addresses looked up (getaddrinfo) on the thread pool: each 9 numbers into out, at most capacity. */
int64_t yel_pool_lookup(int64_t name, int64_t out, int64_t capacity);
bool yel_request_done(int64_t request);
int64_t yel_request_result(int64_t request);
int64_t yel_request_error(int64_t request);
yunit yel_request_free(int64_t request);

/** Sockets that never block (EAGAIN where a call would). */
int64_t yel_host_nonblocking(int64_t fd);
int64_t yel_host_socket_send(int64_t fd, int64_t at, int64_t count);
int64_t yel_host_socket_receive(int64_t fd, int64_t at, int64_t count);
int64_t yel_host_socket_listen(int64_t fd, int64_t backlog);
bool yel_host_socket_is_listening(int64_t fd);
int64_t yel_host_socket_accept(int64_t fd);
int64_t yel_host_socket_shutdown_write(int64_t fd);
int64_t yel_host_socket_error(int64_t fd);
int64_t yel_host_e_again(void);
int64_t yel_host_e_in_progress(void);

/** Up to count bytes of fd at at (read(2)): how many, 0 at its end, -1 for an error. */
int64_t yel_host_read(int64_t fd, int64_t at, int64_t count);

/** count bytes at at written to stdout or stderr (2 for stderr), after what print wrote, and flushed. */
int64_t yel_host_write_out(int64_t fd, int64_t at, int64_t count);

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

