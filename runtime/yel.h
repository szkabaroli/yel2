// The C runtime of compiled yel: the C types its values are made of, and the C functions (and
// macros) a program's extern funcs name (runtime/prelude.yel declares them). This header declares
// them, included by every compiled program; runtime/yel.c defines them, compiled once for a target and
// linked with each program (build/yel.o natively, build/yel-wasm.o for wasm). What stays here is what
// a program's C must see: types, macros, and the small static inline helpers.
//
// Types are checked, so values carry none: an s32 is an int32_t (and so on for s8 to u64, f32 and
// f64), a bool a bool, a char a uint32_t (its code point), a string a ystr (UTF-8 bytes, by value),
// unit a yunit. A list is a ylist of items of one size, a map a ymap of values of one size; the
// compiler gives the size, and casts an item to its C type. Records, variants, options, results and
// tuples are structs the compiler declares. Values show as WAVE, the Component Model's text for
// them. Strings' bytes, lists, maps, records and variants live on a garbage-collected heap (the
// collector, below).
#ifndef YEL_H
#define YEL_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint8_t yunit;

#define Y_NORETURN __attribute__((noreturn))

typedef struct ystr {
	int64_t len;
	const char *data;
} ystr;

// what marks the pointers in one value of a type (a list's item, a map's value), given its address;
// NULL for a type that holds none
typedef void (*y_scan)(void *value);

// a list: len items of size bytes each, scan marking one
typedef struct ylist {
	int64_t len, cap, size;
	char *items;
	y_scan scan;
} ylist;

// a map: its entries in the order they were made (keys[i], values[i]; hashes[i] 0 once removed),
// found through index, open addressing on a key's hash (index[j]: an entry's number + 1, 0 for none).
// A key's type gives the hash and the equality (the compiler's, for that type: == as it is)
typedef struct ymap {
	int64_t len, used, cap, ksize, vsize, icap;
	int64_t *index;
	uint64_t *hashes;
	char *keys, *values;
	y_scan kscan, vscan;
	uint64_t (*hash)(const void *key);
	bool (*eq)(const void *a, const void *b);
} ymap;

Y_NORETURN void y_die(const char *message);

// memory the runtime keeps for itself (never collected)
void *y_alloc(size_t bytes);

// ---------------------------------------------------------------- the collector
//
// Precise and non-moving: mark and sweep. Every heap object has a one-word header, its trace (the C
// function that marks the pointers it holds; NULL for bytes). A small object lives in a 64 KiB page
// of one size class; a large one has pages of its own. A table maps any address in a page to the
// page, so a pointer into the middle of an object (a substring's bytes) keeps it alive too.
//
// The roots are the compiled funcs' frames and the runtime's own (y_root). A func whose locals
// hold pointers keeps a copy of each in a slot of its frame (y_frame, y_top the innermost), the
// same on native and on wasm, where a local in a wasm local could not be seen. A collection runs
// in y_new, when the heap has doubled since the last one left it; YEL_GC_STRESS=n instead runs one
// every n allocations (and fills what it frees with 0xAB), to find a missing root. Under Y_NO_GC
// (C made before the collector, which keeps no frames) none runs.
//
// A component of exports only (a reactor) collects only between turns (y_turns, set by its
// exports' y_ready_turns), where an export returns to the host and nothing of it is on the stack
// (y_turn_end): its funcs keep no frames, which leaves their locals in wasm locals (a third of its
// code). A turn's garbage lasts until the turn ends; under YEL_GC_STRESS every turn's end collects.

typedef void (*y_trace)(void *obj);

// a record a func keeps on its stack (it never escapes the func): traced from the frame, as it is
// no heap object
typedef struct y_stack_obj {
	void *obj;
	y_trace trace;
} y_stack_obj;

typedef struct y_frame {
	struct y_frame *prev;
	int64_t n;
	void **slots;
	int64_t m;
	y_stack_obj *objs;
} y_frame;

extern y_frame *y_top;

// a frame of n slots in a runtime function, for the objects it holds while it allocates again
#define Y_FRAME(n)                                   \
	void *ys_[n] = { 0 };                            \
	y_frame yf_ = { y_top, (n), ys_, 0, NULL };    \
	y_top = &yf_
#define Y_POP() (y_top = yf_.prev)

#define Y_PAGE_SHIFT 16
#define Y_PAGE ((size_t)1 << Y_PAGE_SHIFT)
#define Y_NCLASS 24
#define Y_LARGE 255u

typedef union y_head {
	y_trace trace;
	uint64_t pad;
} y_head;

// a page: its size class's objects (or, for a large object, where it is); live and marks have a
// bit for each object
typedef struct y_page {
	uint32_t cls, size, used;
	char *first, *end, *bump;
	void *free;
	struct y_page *next;
	size_t bytes;
	uint64_t live[Y_PAGE / 16 / 64], marks[Y_PAGE / 16 / 64];
} y_page;

extern const uint32_t y_class_size[Y_NCLASS];
extern uint8_t y_class_of[8192 / 16 + 1];
extern y_page *y_pages[Y_NCLASS], *y_tails[Y_NCLASS], *y_cursor[Y_NCLASS], *y_larges;
// a collection runs once the heap has doubled (or quadrupled: see y_collect) since the last one left
// it, and never below
// y_heap_min (YEL_GC_MIN_MB, default 4): a program that keeps most of what it makes (a compile)
// runs faster with more (fewer collections), a component stays smaller with less
extern size_t y_heap_bytes, y_heap_min;
extern long y_stress, y_stress_left, y_collections, y_verbose;
// while it is not 0 nothing is collected: the JSON decoder builds a value no root holds yet
extern long y_gc_hold;
extern size_t y_peak_bytes;
extern void **y_roots[16];
extern int y_nroots;

void y_root(void **p);

// the runtime's own C tables that name heap objects (the timers, the host's waits): each marks
// what it names when the collector runs, so nothing they name is freed under them
extern void (*y_tracers[8])(void);
extern int y_ntracers;

void y_tracer(void (*trace)(void));
// the program's globals made once, before its first code (main's, an export's), then marked
void y_globals(void (*init)(void), void (*mark)(void));
// an exported resource's objects by rep (a table for each resource, by its index): one given the
// host, the one a rep names, one let go
int32_t y_res_new(int32_t table, void *object);
void *y_res_get(int32_t table, int32_t rep);
void y_res_free(int32_t table, int32_t rep);

// ---- the page table: a page's address to the page (never shrinks; a freed large object's
// pages map to NULL)

typedef struct y_pte {
	uintptr_t key;
	y_page *page;
} y_pte;

extern y_pte *y_pt;
extern size_t y_pt_cap, y_pt_len;
// the page last looked up: pointers marked one after another are mostly in one page
extern uintptr_t y_pt_last_key;
extern y_page *y_pt_last;

size_t y_pt_index(uintptr_t key, size_t cap);

void y_pt_put(uintptr_t key, y_page *page);

y_page *y_page_at(const void *p);

// the object p points into (its header) and its index in its page, or NULL
y_head *y_object(const void *p, y_page **page, size_t *index);

// ---- marking

extern y_head **y_stack;
extern size_t y_stack_len, y_stack_cap;

/** Marks the object p points into (if any) live, and queues it for its trace. */
void y_mark(const void *p);

// a list's items and a map's values are marked through these: a pointer, or a string's bytes
void y_scan_ptr(void *value);
void y_scan_str(void *value);

void y_collect(void);

// ---- allocating

void *y_page_take(y_page *pg);

void *y_pages_alloc(size_t bytes);

y_page *y_page_new(int c);

y_head *y_new_large(size_t need);

void *y_new_block(size_t bytes, y_trace trace, bool zero);

/** A new heap object of bytes (zeroed), its pointers marked by trace (NULL: it holds none). */
void *y_new(size_t bytes, y_trace trace);

/** New heap bytes, not zeroed: for what the caller fills at once (a string's bytes, a list's items
 *  up to its length). */
void *y_new_bytes(size_t bytes);

void *y_new_block(size_t bytes, y_trace trace, bool zero);

// ---------------------------------------------------------------- strings

ystr y_str_of(const char *bytes, int64_t len);

// a float's text, the shortest that reads back as it (nan, inf, -inf): std's format.write-f64
ystr yel_f64_text(double v);

ystr yel_f32_text(float v);

// ---- as text (a string or char in an interpolation)

// ---------------------------------------------------------------- buffers

/**
 * A buffer: bytes gathered, append-only. A string it hands out shares its bytes (none of them is
 * written again); growing moves the bytes to a new block, the old one kept by what shares it.
 */
typedef struct ybuffer {
	int64_t len, cap;
	char *data;
} ybuffer;

ylist *y_list_new(int64_t cap, int64_t size, y_scan scan);

void y_trace_buffer(void *obj);

ybuffer *yel_buffer_new(int64_t cap);

// the bytes so far as a string: shared, not copied
ystr yel_buffer_string(ybuffer *b);

// ---------------------------------------------------------------- type descriptors

// a type as the program holds its values (one static table for each type the compiler shows): what
// one walker reads to show any value. A record's or variant's value is a pointer to its object (its
// offsets are into the object); an option's, result's, tuple's, union's and anonymous record's is a
// struct held by value
enum {
	Y_K_UNIT, Y_K_BOOL, Y_K_CHAR, Y_K_SIGNED, Y_K_UNSIGNED, Y_K_F32, Y_K_F64, Y_K_STRING, Y_K_BUFFER,
	Y_K_MAP, Y_K_LIST, Y_K_FIXED, Y_K_OPTION, Y_K_RESULT, Y_K_TUPLE, Y_K_ANON_RECORD, Y_K_RECORD,
	Y_K_VARIANT, Y_K_ENUM, Y_K_FLAGS, Y_K_HANDLE, Y_K_UNION, Y_K_FUNC, Y_K_FUTURE, Y_K_SEQ, Y_K_STREAM,
	// a record (a value): held in place as an anonymous record is, shown as a declared one is
	Y_K_VALUE_RECORD,
};

typedef struct ytype {
	uint8_t kind;
	// a fixed-length list's length; the count of fields, cases, kinds or flags
	uint32_t count;
	// sizeof the value as the program holds it (a list's items are this far apart)
	uint32_t size;
	// a list's item; each field's, case's (NULL: no payload) or kind's type; an option's value, a
	// result's ok and err
	const struct ytype *const *parts;
	// where each part is: an option's has and value, a result's is_err, ok and err, a union's or
	// variant's tag then each case
	const uint32_t *offsets;
	// each field's, case's or flag's name; a handle's resource
	const char *const *names;
	// what the JSON decoder needs to make a value (NULL where the program decodes none)
	const struct ydecode *decode;
} ytype;

typedef struct ydecode {
	// a record's or variant's object: its size, and its trace
	uint32_t object;
	y_trace trace;
	// a list's items' scan, a map's values'
	y_scan scan;
	// a variant's cases without a payload: their objects
	void *const *singles;
} ydecode;

// whether stderr may be colored: a terminal, and NO_COLOR not set
bool yel_stderr_color(void);

// v, which the C compiler must take as unknown (a benchmark's barrier)
#define yel_black_box(T, S, v) ({ T y_bb_ = (v); __asm__ volatile("" : : "r"(&y_bb_) : "memory"); y_bb_; })

// a match no arm fit: the program stops, showing the value
Y_NORETURN void yel_no_match(ystr shown);

// ---------------------------------------------------------------- operators

// an integer type's + - * / % << >> and negation: they wrap (as WebAssembly's do), a shift takes
// its count modulo the width, and / or % by zero stops the program
#define Y_INT_OPS(N, T, U, BITS) \
	static inline T y_add_##N(T a, T b) { return (T)((U)a + (U)b); } \
	static inline T y_sub_##N(T a, T b) { return (T)((U)a - (U)b); } \
	static inline T y_mul_##N(T a, T b) { return (T)((U)a * (U)b); } \
	static inline T y_neg_##N(T a) { return (T)((U)0 - (U)a); } \
	static inline T y_shl_##N(T a, int64_t b) { return (T)((U)a << (b & (BITS - 1))); } \
	static inline T y_shr_##N(T a, int64_t b) { return (T)(a >> (b & (BITS - 1))); } \
	static inline T y_div_##N(T a, T b) { \
		if (b == 0) y_die("division by zero"); \
		if ((T)-1 < 0 && b == (T)-1) return y_neg_##N(a); \
		return (T)(a / b); \
	} \
	static inline T y_mod_##N(T a, T b) { \
		if (b == 0) y_die("division by zero"); \
		if ((T)-1 < 0 && b == (T)-1) return 0; \
		return (T)(a % b); \
	}
Y_INT_OPS(s8, int8_t, uint32_t, 8)
Y_INT_OPS(s16, int16_t, uint32_t, 16)
Y_INT_OPS(s32, int32_t, uint32_t, 32)
Y_INT_OPS(s64, int64_t, uint64_t, 64)
Y_INT_OPS(u8, uint8_t, uint32_t, 8)
Y_INT_OPS(u16, uint16_t, uint32_t, 16)
Y_INT_OPS(u32, uint32_t, uint32_t, 32)
Y_INT_OPS(u64, uint64_t, uint64_t, 64)

// ---------------------------------------------------------------- lists

void y_trace_list(void *obj);

/** The bytes of cap items of size bytes (a size no address space holds: out of memory). */
size_t y_list_bytes(int64_t size, int64_t cap);

/** An empty list of items of size bytes (scan marks one; NULL when they hold no pointers). One
 *  block: the list, then room for its first cap items (at least 4; a byte at least, so items points
 *  inside it). Grown past them, its items move to a block of their own (std's list.grow); the
 *  collector marks items either way (an interior pointer marks the block it is in). */
ylist *y_list_new(int64_t cap, int64_t size, y_scan scan);

/** A list literal: its n items. */
ylist *y_list_of(int64_t n, int64_t size, const void *items, y_scan scan);
ylist *y_list_static(const ylist *data, const struct ytype *t);
ylist *y_list_of_data(int64_t n, int64_t size, const void *items, y_scan scan);

void *y_list_push_slot(ylist *l);

// the prelude's generic list funcs: T, the item's C type, and S, what scans one, first
// ---------------------------------------------------------------- as: the conversions C leaves
// undefined, defined (a float to an integer saturates, NaN to 0; f64 past f32's range to an
// infinity; an integer that is no Unicode scalar value to U+FFFD)

#define Y_SAT(N, T, LO, HI)                                    \
	static inline T y_sat_##N(double v) {                      \
		if (v != v) return 0;                                  \
		if (v <= (double)(LO)) return (LO);                    \
		if (v >= (double)(HI)) return (HI);                    \
		return (T)v;                                           \
	}
Y_SAT(s8, int8_t, INT8_MIN, INT8_MAX)
Y_SAT(s16, int16_t, INT16_MIN, INT16_MAX)
Y_SAT(s32, int32_t, INT32_MIN, INT32_MAX)
Y_SAT(s64, int64_t, INT64_MIN, INT64_MAX)
Y_SAT(u8, uint8_t, 0, UINT8_MAX)
Y_SAT(u16, uint16_t, 0, UINT16_MAX)
Y_SAT(u32, uint32_t, 0, UINT32_MAX)
Y_SAT(u64, uint64_t, 0, UINT64_MAX)

static inline float y_f64_to_f32(double v) {
	// rounded to nearest: past FLT_MAX and half its ulp (0x1.ffffffp127: a tie, to even, is up) is
	// an infinity; anything nearer rounds into f32's range (C leaves the conversion of a value
	// out of range undefined, so that is said here)
	if (v >= 0x1.ffffffp127) return INFINITY;
	if (v <= -0x1.ffffffp127) return -INFINITY;
	return (float)v;
}

static inline uint32_t y_to_char(int64_t v) {
	return v < 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF) ? 0xFFFD : (uint32_t)v;
}

/** An index past a list's ends: the program stops, saying so. */
Y_NORETURN void y_index_out_of_range(int64_t i, int64_t n);

/** A fixed-length list's index, checked against its length n. */
static inline int64_t y_bound(int64_t i, int64_t n) {
	if (i < 0 || i >= n) y_index_out_of_range(i, n);
	return i;
}

// ---------------------------------------------------------------- maps

// zeroed bytes of the heap (a map's index, hashes, keys and values start empty)
void *y_new_zeroed(size_t bytes);

// a hash of a value's bits (a number, a char, a pointer: what == compares)
uint64_t y_hash_bits(uint64_t v);

// two hashes as one (a tuple's, a record's)
uint64_t y_hash_mix(uint64_t h, uint64_t v);

/** An address's hash (a list's, a record's: what == compares of it), and a float's (0.0 and -0.0
 *  one hash, as == has them equal). */
uint64_t y_hash_address(const void *p);
uint64_t y_hash_f32(float f);
uint64_t y_hash_f64(double f);

void y_trace_map(void *obj);

/** An empty map with room for capacity entries (its index for twice as many): keys of ksize bytes
 *  (kscan marks one; hash and eq: its type's), values of vsize. Its table is std's (map.yel). */
ymap *y_map_make(int64_t ksize, y_scan kscan, uint64_t (*hash)(const void *), bool (*eq)(const void *, const void *),
	int64_t vsize, y_scan vscan, int64_t capacity);

// the prelude's generic map funcs: for each type argument its C type and what scans one first,
// and a key's (K) its hash and equality too. A key is given by its address (a copy of it)
// a set: a map whose values are ()

// ---------------------------------------------------------------- strings: bytes

// a float with digits after the point, rounded (std's format.fixed): nan, inf and -inf as
// interpolation shows them
ystr yel_fixed(double value, int32_t digits);

// ---------------------------------------------------------------- the process

extern ylist *y_args;

yunit yel_print(ystr s);

yunit yel_eprint(ystr s);

/** The target triple this runtime was built for (yelc's default for --backend bitcode; "" for one
 * it does not know). */
ystr yel_host_triple(void);
ystr yel_executable_path(void);

/** What main does last: stdout flushed, main's value the exit code. */
int y_finish(int64_t code);

Y_NORETURN yunit yel_panic(ystr message);

char *y_cstr(ystr s);

/** A decimal float's value (as a float literal is written), for the compiler's keys. */
double yel_parse_f64(ystr s);

// YEL_GC_STATS: how the collector did, on stderr at exit
void y_stats(void);

// the program's arguments as main has them (the native WASI host's get-arguments reads them)
extern int y_argc;
extern char **y_argv;

// ---------------------------------------------------------------- the canonical ABI
//
// What the compiler's lowering and lifting (and a component's host) share: a string as the ABI has
// it, memory lowered for one call (freed after it, or, for an export's result, in its post-return),
// the allocator the host writes into the component's memory with, and a slot's bits as a float.

// a component's export (natively: a plain C symbol, the same function a C host calls)
#if defined(__wasm__)
#define Y_EXPORT(name) __attribute__((export_name(name)))
#else
#define Y_EXPORT(name)
#endif

typedef struct yabi_str {
	char *ptr;
	size_t len;
} yabi_str;

extern void **y_abi_temps;
extern size_t y_abi_ntemps, y_abi_captemps;

void *y_abi_temp(size_t bytes);

void y_abi_free_temps(void);
void y_abi_free_host(void *ptr, size_t len);

/* ================================================================ async: every target's, then the
target's own reactor and host */
#include "async.h"
#if defined(__wasm__)
#include "async-wasi.h"
#else
#include "async-native.h"
#endif

/** A char the other side gave: a Unicode scalar value, else a trap (the canonical ABI's rule). */
static inline uint32_t y_abi_char(uint32_t c) {
	if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) y_die("a char that is not a Unicode scalar value crossed the boundary");
	return c;
}

/** A case's tag the other side gave: one of n, else a trap. */
static inline int32_t y_abi_tag(uint32_t tag, uint32_t n) {
	if (tag >= n) y_die("a tag out of its type's range crossed the boundary");
	return (int32_t)tag;
}

/** A string the host wrote (into memory from cabi_realloc): copied to the heap, the host's freed. */
ystr y_abi_lift_str(yabi_str *s);

/** What crossed the boundary that the type has not: the program stops, saying which (0: a value
 *  outside its type, 1: a case the union has not, 2: a case the variant has not, 3: a case the
 *  enum has not, 4: a char no Unicode scalar value, 5: a tag past its type's cases, 6: a future's
 *  writer gone before it wrote). */
Y_NORETURN void y_abi_bad(int32_t what);

/** What print left in libc's stdout buffer written (before the host's exit, which never returns). */
void y_flush_stdout(void);

static inline float y_f32_of(uint64_t bits) {
	uint32_t b = (uint32_t)bits;
	float f;
	memcpy(&f, &b, 4);
	return f;
}

static inline double y_f64_of(uint64_t bits) {
	double f;
	memcpy(&f, &bits, 8);
	return f;
}

static inline uint64_t y_bits_f32(float f) {
	uint32_t b;
	memcpy(&b, &f, 4);
	return b;
}

static inline uint64_t y_bits_f64(double f) {
	uint64_t b;
	memcpy(&b, &f, 8);
	return b;
}

#if defined(__wasm__)
// what the host writes into the component's memory (a string, a list) comes from here: plain
// memory, which lifting copies to the heap and frees
__attribute__((export_name("cabi_realloc"))) void *cabi_realloc(void *old, size_t old_size, size_t align, size_t new_size);
#else
#include "wasi-native.h"
#endif

// a component of exports only (no main): made ready by its first export's call
extern bool y_started;
/** The program's arguments (argv after the program), for main. */
ylist *y_start(int argc, char **argv);
void y_ready(void);
extern bool y_turns;
void y_ready_turns(void);

// a turn's end (an export's post-return: nothing of the component is on the stack): the
// collector runs when the heap has grown past half of what starts one
void y_turn_end(void);

// a count from the environment: a decimal, 0 for anything else (no overflow: strtol clamps)
long y_env_count(const char *s);

// std:process: a child process, how one ended, and a process's end as it is
int64_t yel_process_fork(void);
int64_t yel_process_wait(int64_t child);
int64_t yel_process_run(ystr arguments, int64_t count, ystr dir, ystr environment, int64_t variables, ystr output);
int64_t yel_process_start(ystr arguments, int64_t count, ystr dir, ystr environment, int64_t variables, ystr output);
bool yel_process_done(int64_t child);
int64_t yel_process_status(int64_t child);
yunit yel_process_free(int64_t child);
ystr yel_build_entries(ystr dir);
bool yel_build_runnable(ystr at);
int64_t yel_build_parallelism(void);
Y_NORETURN void yel_process_end(int64_t code);

#endif
