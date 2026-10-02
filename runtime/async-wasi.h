// yel's async runtime as a WASI 0.3 component: the host is the reactor. What a task waits for the
// host to complete (a subtask, a stream's or future's read or write) is a waitable, joined to its
// root's waitable set; a component's async export runs its root's tasks and gives the host EXIT or
// WAIT; an async main waits on the host (waitable-set.wait). Streams and futures cross WIT here
// (runtime/async.h holds what every target shares).

/* ---------------------------------------------------------------- the host's waits (WASI 0.3)

A component's async import called (a subtask), or anything else the host completes, is a waitable:
joined to its root's waitable set, it wakes the task waiting for it when the host says it is done
(an event). A component's async export runs its root's tasks until the root is done (task.return,
EXIT) or all of them wait for the host (WAIT on the root's set); the host calls back with each event.
*/
typedef struct { uint32_t waitable; y_async *task; y_async *root; int32_t done; uint32_t code; } y_hostwait;
extern y_hostwait *y_hostwaits;
extern int64_t y_nhostwaits, y_hostwaits_cap;

typedef struct { y_async *root; uint32_t set; void (*finish)(y_async *root); int32_t returned; } y_hostroot;
extern y_hostroot *y_hostroots;
extern int64_t y_nhostroots, y_hostroots_cap;

__attribute__((import_module("$root"), import_name("[waitable-set-new]"))) extern uint32_t y_cm_set_new(void);
__attribute__((import_module("$root"), import_name("[waitable-join]"))) extern void y_cm_join(uint32_t, uint32_t);
__attribute__((import_module("$root"), import_name("[waitable-set-wait]"))) extern uint32_t y_cm_set_wait(uint32_t, uint32_t *);
__attribute__((import_module("$root"), import_name("[subtask-drop]"))) extern void y_cm_subtask_drop(uint32_t);
__attribute__((import_module("$root"), import_name("[waitable-set-drop]"))) extern void y_cm_set_drop(uint32_t);

// the tasks and roots the host's waits name, marked: a task waiting for the host stays until the
// host is done with what it waits for, a root until its call is over (y_host_root_end)
void y_trace_host(void);

y_hostroot *y_host_root(y_async *root);

/** A root's call over (EXIT given): its waitable set dropped, its entry and its waits' let go. */
void y_host_root_end(y_async *root);

/** The task being stepped waits for a waitable the host completes (a subtask). */
void y_async_host_wait(uint32_t waitable);

/** The task being stepped waits, in place of the one that did, for a waitable already waited for
(a read another task began and left). */
void y_async_host_rewait(uint32_t waitable);

/** The code the host completed a waitable with (a stream's or future's: count << 4 | state). */
uint32_t y_async_host_code(uint32_t waitable);

/** A stream's or future's end done waiting for: its entry let go (the end is kept). */
void y_async_host_forget(uint32_t waitable);

y_hostwait *y_host_find(uint32_t waitable);

/** Whether the host is done with a waitable (it woke its task). */
bool y_async_host_done(uint32_t waitable);

/** A subtask done with: let go (its entry, and the host's handle). */
void y_async_host_drop(uint32_t waitable);

uint32_t y_async_host_code(uint32_t waitable);

void y_async_host_forget(uint32_t waitable);

/** Whether any task of a root's waits for the host. */
bool y_async_host_pending(y_async *root);

/** An event from the host: a subtask returned (event 1, code 2) wakes the task waiting for it.
Its root (the call it is for). */
y_async *y_async_host_event(uint32_t event, uint32_t waitable, uint32_t code);

/** The reactor: no task ready, so wait for the host (one event at a time, for a root whose task
waits for it) or sleep until the first timer, and wake what is due. False when nothing will wake
any task (every one waits for another, or for nothing). */
bool yel_async_await(void);

/* ---------------------------------------------------------------- streams and futures across WIT

A stream the host writes is read into its yel stream's items when they run out (yel_stream_fill:
a read of the host's end into a buffer, waited for when blocked, its items lifted). A yel stream
given the host is pumped: a task (the call's) writes its items to the host's writable end as they
come, waits when the host is not reading, and drops the end once the stream is closed. Each type's
builtins and lift or lower are the compiler's (one set for each stream in a WIT func's type). */
#define Y_BLOCKED 0xffffffffu

/** A stream the host writes, as a yel stream (its items lifted as they are read). */
ystream *y_stream_from_host(uint32_t end, uint32_t (*read)(uint32_t, uint8_t *, size_t),
	void (*lift)(ystream *, void *, int32_t), void (*drop)(uint32_t), size_t abi_size, int64_t size, y_scan scan);

typedef struct { y_async h; yunit result; ystream *s; } y_stream_filler;

void y_trace_stream_filler(void *obj);

/** The host's read into the stream done (code: count << 4 | state): its items lifted. */
int32_t y_stream_filled(ystream *s, uint32_t code);

int32_t y_stream_fill_step(void *self);

/** A future done once more of the host's items are read into the stream (or it is closed). */
y_async *yel_stream_fill(ystream *s);

/** A stream no one reads any more (the compiler's, where its local is last used): the host's end
let go (its writer told), or once the read under way is done. */
void y_stream_release(ystream *s);

#define yel_stream_release(T, S, s) y_stream_release(s)
#define yel_stream_from_host(T, S, s) ((s)->host != 0)
#define yel_stream_host_fill(T, S, s) yel_stream_fill(s)

typedef struct {
	y_async h;
	yunit result;
	ystream *s;
	ylist *held;
	uint32_t end;
	uint32_t (*write)(uint32_t, const uint8_t *, size_t);
	void (*lower)(void *items, void *buf, int32_t n);
	void (*drop)(uint32_t);
	size_t abi_size;
	uint8_t *buf;
	int32_t n;
	int32_t off;
	y_async *fill;
} y_stream_pump;

void y_trace_stream_pump(void *obj);

int32_t y_stream_pump_step(void *self);

/** A yel stream given the host: a stream the host writes, none of it read here, is given back as
it is (its end moves, nothing is copied); any other, a new pair (its readable end given the host)
and a task (the call's) writing the stream's items to the writable end. */
uint32_t y_stream_to_host(ystream *s, uint64_t (*new_pair)(void), uint32_t (*write)(uint32_t, const uint8_t *, size_t),
	void (*lower)(void *, void *, int32_t), void (*drop)(uint32_t), size_t abi_size);

