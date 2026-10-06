// yel's async runtime, the part every target shares (runtime/yel.h includes it, then the
// target's: async-native.h, or async-wasi.h for a WASI 0.3 component): tasks, the executor's
// queues, wakers, timers as a list, and streams between tasks. What waits for the world outside
// (the reactor: yel_async_await) is the target's.

/* ================================================================ async

An async func is a state machine: its frame starts with this header (its step, the state it waits
in, whether it is done, whether it is a task of its own, whether it is in the ready queue), then
its result, then its locals. A future is its frame. The executor (yel:std's async.run-tasks) steps
the tasks that are ready, a turn each; one that cannot go on parks, and what it waits for wakes it
(back into the ready queue): a task it waits for once that is done, a timer, a stream. When none is
ready, the reactor (here: timers) sleeps until one is. Everything is kept rooted here, so the
frames live while they run. */
#include <time.h>

typedef struct y_async {
	int32_t (*step)(void *self);
	int32_t state;
	int32_t done;
	int32_t spawned;
	int32_t queued;
	// when it was done (a count: which of several waited for was done first)
	int64_t order;
	// the root task it runs for (an async main's, or a component's async export call's): the
	// tasks a call runs are its own
	struct y_async *owner;
	// a future the host writes: its end let go, when no one will wait for it (else NULL)
	void (*release)(struct y_async *self);
	// a task's async context: its values of the program's @(context) globals, while another runs
	// (taken from its starter's when started; NULL for none: no such globals, or not a task)
	void *context;
	// a task's number (from 1, once started) and its async func's name: what YEL_ASYNC_TRACE tells
	int64_t id;
	const char *name;
} y_async;

/** YEL_ASYNC_TRACE: the tasks still waiting, and on what (a deadlock found: told before the program
stops), or a hint to set it; and those never done once the program's main is (at its end). */
yunit yel_async_trace_deadlock(void);
yunit yel_async_trace_end(void);

/** A global made an async context's (@(context)): each task its own value of it, a task started taking
its starter's as it is then. Its place, size and scan (NULL: it holds no pointer); made known once, by
the program's y_globals_init. */
void y_context_global(void *place, size_t size, void (*scan)(void *));

extern int64_t y_async_order;

extern ylist *y_async_list;    // every started task (rooted)
extern ylist *y_async_ready;   // the tasks to step, in order (from y_async_head on)
extern int64_t y_async_head;
extern ylist *y_async_waits;   // pairs: a started task, a task waiting for it
extern y_async *y_async_now;   // the task being stepped: what a wait parks

typedef struct { int64_t at; y_async *task; } y_timer;
extern y_timer *y_timers;
extern int64_t y_ntimers, y_timers_cap;

ylist *y_async_rooted(ylist **l);

ylist *yel_async_tasks(void);

/** A task back in the ready queue (once; not once done). */
void y_wake(y_async *t);

/** The tasks waiting for t (it is done) woken. */
void y_async_finished(y_async *t);

/** A task started: its own, the executor's to step (ready at once); its owner the starter's (a
root's: itself). */
y_async *yel_async_spawn(y_async *t);

/** Whether a task of a root's is ready, and the first such taken out of the queue (a component's
call runs only its own: the others' are stepped by their calls). */
bool yel_async_has_ready_of(y_async *root);

y_async *yel_async_take_ready_of(y_async *root);

/** The next ready task (the queue's first; its room let go once all are taken). */
y_async *yel_async_take_ready(void);

/** A task stepped by the executor (the one a wait in it parks); done: its waiters woken. */
yunit yel_async_run(y_async *t);

/** What a wait does with what it waits for: a task of its own is stepped by the executor (the
waiter parks until it is done); anything else (an async call, a timer, a stream's read) is stepped
here. Whether it is done. */
bool yel_async_step(y_async *f);

/** wait a | b | …: each stepped (or waited for), and which was done first (its index; -1: none
yet). */
#include <stdarg.h>
int32_t yel_async_first(int32_t n, ...);

/** The same, of n tasks in an array (what the bitcode backend calls: no C varargs). */
int32_t yel_async_first_of(int32_t n, y_async **tasks);

/** Whether a task is ready (std's async.has-ready, as a function). */
bool yel_async_has_ready(void);

/** A generator stepped to its next value (in its frame's result); false once it is done. */
bool y_seq_next(y_async *g);

/** A future no one will wait for (the compiler's, where its local is last used): a host's end let
go, unless it is being read (then that read lets it go). Any other future: nothing to do. */
#define yel_future_release(T, S, f) y_future_release(f)
void y_future_release(y_async *f);

/** A task's release (its end let go, when no one will wait for it) done by its own step: stepped
 *  once in the state Y_RELEASE_STATE (a compiled task that holds a host's end has one). */
#define Y_RELEASE_STATE 1000000
void y_async_release_by_step(y_async *f);

/** An async export's call driven, what its lift and its callback give the host: its root's ready
 *  tasks run (run: std's async.run-ready-of); done, its result given once (its finish: task.return,
 *  set by y_export_finish); over (EXIT, 0) once nothing of it is left; else WAIT on its root's
 *  waitable set while a task waits for the host, or a deadlock. */
int32_t y_export_drive(y_async *root, bool (*run)(y_async *));
void y_export_finish(y_async *root, void (*finish)(y_async *));

/** The task being stepped (what a parked wait wakes). */
y_async *yel_async_current(void);

yunit yel_async_wake(y_async *t);

/** A future done the second time it is stepped, its task woken at once (a turn for the others). */
int32_t y_async_yield_step(void *self);

y_async *yel_async_yield(void);

/** A future done the second time it is stepped, nothing woken: its task waits until something
wakes it (a stream, a timer: what parked it). */
int32_t y_async_park_step(void *self);

y_async *yel_async_park(void);

/** Milliseconds on a clock that only goes on. */
int64_t yel_async_now_ms(void);
int64_t yel_wall_ms(void);

/** The task being stepped, woken at a time. */
// the timers' tasks, marked: a task a timer names stays until the timer is due (waking a task that
// is done by then does nothing)
void y_trace_timers(void);

yunit yel_async_timer(int64_t at);

/** The started tasks that are done, let go (a waiter still holding one keeps its frame). */
yunit yel_async_prune(void);

/* A stream: its items in order (a list, at most cap of them), whether its writer closed it, and
the task waiting to read (its reader) and to write (its writer). stream<T> and writer<T> are its
two ends, one object. */
typedef struct ystream {
	ylist *items;
	int64_t cap;
	int32_t closed;
	y_async *reader;
	y_async *writer;
	// a stream the host writes (WASI 0.3): its readable end (0: none), and the builtins and the
	// lift for this stream's type (its items as the host lays them out)
	uint32_t host;
	uint32_t (*host_read)(uint32_t end, uint8_t *buf, size_t n);
	void (*host_lift)(struct ystream *s, void *buf, int32_t n);
	void (*host_drop)(uint32_t end);
	size_t abi_size;
	// a read of the host's end under way (into host_buf: the stream's, not the read's, so a read
	// left half way, as by a wait a | b another won, is the next read's to finish), and its end to
	// be let go once it is done
	int32_t host_busy;
	int32_t host_release;
	uint8_t *host_buf;
} ystream;

/** The task being stepped waits for a stream's items (its reader), or for room in it (its writer):
the stream wakes it. */
void yel_stream_park_reader(ystream *s);
void yel_stream_park_writer(ystream *w);

void y_trace_stream(void *obj);

ystream *y_stream_new(int64_t size, y_scan scan, int64_t cap);

#define yel_writer_close(T, S, w) ((w)->closed = 1, y_wake((w)->reader), (w)->reader = NULL, (yunit)0)

/** The same as a function (what the bitcode backend calls: no C macros there). */
yunit y_writer_close(ystream *w);

/** A list's first item, taken out (the rest moved up), into out (the caller's: a T). */
void *y_list_take_first(ylist *l, void *out);

/** Whether a task of a root's (but the root) is not done: a pump still writing to the host. */
bool y_async_owned_live(y_async *root);

/** The reactor (the target's): no task ready, so wait for what will wake one; false when nothing
will. */
bool yel_async_await(void);

