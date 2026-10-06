// The runtime's definitions (yel.h declares them): compiled once for a target and linked with
// each program (the C backend's, the bitcode backend's alike).
#include "yel.h"

// Y_HOSTED: a component built for a host that grants only the streams and the clocks (the shell's
// UI hosts: no wasi:cli/environment, no wasi:cli/exit). Nothing is read from the environment (the
// YEL_GC_* switches, NO_COLOR), and what stops the program traps: the host sees the component's
// call fail, as a component's fatal error is told
#if defined(Y_HOSTED)
#define y_getenv(name) ((const char *)0)
#define y_stop() __builtin_trap()
#else
#define y_getenv(name) getenv(name)
// a forked child (std:process) stops without its parent's teardown (libuv's thread pool, which
// exit's handlers join, is the parent's): _exit, its output flushed
static bool y_forked = false;
Y_NORETURN static void y_stop_now(void) {
	if (y_forked) {
		fflush(NULL);
		_exit(1);
	}
	exit(1);
}
#define y_stop() y_stop_now()
#endif

// what the runtime writes on stdout (fd 1) or stderr (2): false where it could not. A component's
// (Y_HOSTED, WASI 0.2) goes on the host's streams themselves, never through stdio (its buffers,
// locks and descriptor table, and the stdin and terminal imports its start asks the host for);
// what it writes is written at once, so there is nothing to flush
#if defined(Y_HOSTED) && defined(__wasip2__)
#include <wasi/wasip2.h>
static bool y_out(int fd, const void *data, size_t len) {
	static streams_own_output_stream_t streams[2];
	static bool opened[2];
	const int k = fd == 2;
	if (!opened[k]) {
		streams[k] = k ? stderr_get_stderr() : stdout_get_stdout();
		opened[k] = true;
	}
	const streams_borrow_output_stream_t to = streams_borrow_output_stream(streams[k]);
	const uint8_t *at = data;
	while (len > 0) {
		// (at most 4096 bytes a write, as the stream takes them)
		const size_t n = len > 4096 ? 4096 : len;
		wasip2_list_u8_t chunk = { (uint8_t *)at, n };
		streams_stream_error_t error;
		if (!streams_method_output_stream_blocking_write_and_flush(to, &chunk, &error)) {
			if (error.tag == STREAMS_STREAM_ERROR_LAST_OPERATION_FAILED) io_error_error_drop_own(error.val.last_operation_failed);
			return false;
		}
		at += n;
		len -= n;
	}
	return true;
}
#define y_out_flush() ((void)0)
#else
static bool y_out(int fd, const void *data, size_t len) { return fwrite(data, 1, len, fd == 2 ? stderr : stdout) == len; }
#define y_out_flush() fflush(stdout)
#endif

static void y_out_text(int fd, const char *text) { (void)y_out(fd, text, strlen(text)); }

Y_NORETURN void y_die(const char *message) {
	y_out_flush();
	// (no printf: a guest's build is smaller without it)
	y_out_text(2, "yel: ");
	y_out_text(2, message);
	y_out_text(2, "\n");
	y_stop();
}

// ---- a component's memory (Y_HOSTED on wasm), in place of libc's dlmalloc (a tenth of a small
// component's code). Wasm's memory only grows, so what an allocator there needs is reuse, never
// giving memory back. A block of up to 32 KiB is one of a size class (a power of two from 32
// bytes, its first 16 bytes saying which), cut from a 64 KiB page and kept on its class's list
// once free; a bigger one (or one aligned to more than 16 bytes: the collector's pages) is a run of
// whole pages, its length in a table by its first page, kept once free on a list in address order,
// merged with the free runs next to it. (Y_MEMORY_TEST: the same over a native arena, to test it)
#if (defined(Y_HOSTED) && defined(__wasm__)) || defined(Y_MEMORY_TEST)
#ifndef Y_MEMORY_TEST
#define Y_MEMORY_GROW(pages) __builtin_wasm_memory_grow(0, pages)
#define Y_MEMORY_BASE ((uintptr_t)0)
#endif
#define Y_MEM_PAGE ((size_t)65536)
#define Y_MEM_CLASSES 11
#define Y_MEM_HEAD ((size_t)16)

typedef struct y_mem_run {
	struct y_mem_run *next;
	size_t pages;
} y_mem_run;

static void *y_mem_free[Y_MEM_CLASSES];
static y_mem_run *y_mem_runs;
static uint16_t y_mem_run_pages[65536];

static size_t y_mem_page(const void *p) { return ((uintptr_t)p - Y_MEMORY_BASE) / Y_MEM_PAGE; }

// a run of pages: the first free run long enough (the rest of it left free), else memory grown
static void *y_mem_run_take(size_t pages) {
	if (pages == 0 || pages > 65535) return NULL;
	y_mem_run **link = &y_mem_runs;
	for (y_mem_run *run = y_mem_runs; run; link = &run->next, run = run->next) {
		if (run->pages < pages) continue;
		if (run->pages == pages) *link = run->next;
		else {
			y_mem_run *rest = (y_mem_run *)((char *)run + pages * Y_MEM_PAGE);
			rest->next = run->next;
			rest->pages = run->pages - pages;
			*link = rest;
		}
		y_mem_run_pages[y_mem_page(run)] = (uint16_t)pages;
		return run;
	}
	const size_t was = (size_t)Y_MEMORY_GROW(pages);
	if (was == SIZE_MAX) return NULL;
	void *made = (void *)(Y_MEMORY_BASE + was * Y_MEM_PAGE);
	y_mem_run_pages[y_mem_page(made)] = (uint16_t)pages;
	return made;
}

static void y_mem_run_give(void *p) {
	y_mem_run *run = p;
	run->pages = y_mem_run_pages[y_mem_page(p)];
	y_mem_run_pages[y_mem_page(p)] = 0;
	y_mem_run *before = NULL;
	y_mem_run *after = y_mem_runs;
	while (after && after < run) {
		before = after;
		after = after->next;
	}
	run->next = after;
	if (after && (char *)run + run->pages * Y_MEM_PAGE == (char *)after) {
		run->pages += after->pages;
		run->next = after->next;
	}
	if (!before) y_mem_runs = run;
	else if ((char *)before + before->pages * Y_MEM_PAGE == (char *)run) {
		before->pages += run->pages;
		before->next = run->next;
	} else before->next = run;
}

static void *y_mem_take(size_t bytes) {
	if (bytes > ((size_t)32 << (Y_MEM_CLASSES - 1)) - Y_MEM_HEAD) return y_mem_run_take((bytes + Y_MEM_PAGE - 1) / Y_MEM_PAGE);
	int c = 0;
	while (((size_t)32 << c) - Y_MEM_HEAD < bytes) c++;
	char *block = y_mem_free[c];
	if (block) y_mem_free[c] = *(void **)block;
	else {
		// a page of the class's blocks: the first given, the others on its list
		char *page = y_mem_run_take(1);
		if (!page) return NULL;
		const size_t size = (size_t)32 << c;
		for (size_t at = Y_MEM_PAGE - size; at > 0; at -= size) {
			*(void **)(page + at) = y_mem_free[c];
			y_mem_free[c] = page + at;
		}
		block = page;
	}
	*(size_t *)block = (size_t)c;
	return block + Y_MEM_HEAD;
}

// how many bytes a block given out holds (a run's: its pages)
static size_t y_mem_held(const void *p) {
	if (((uintptr_t)p - Y_MEMORY_BASE) % Y_MEM_PAGE == 0) return y_mem_run_pages[y_mem_page(p)] * Y_MEM_PAGE;
	return ((size_t)32 << *(const size_t *)((const char *)p - Y_MEM_HEAD)) - Y_MEM_HEAD;
}

static void y_mem_give(void *p) {
	if (!p) return;
	if (((uintptr_t)p - Y_MEMORY_BASE) % Y_MEM_PAGE == 0) {
		y_mem_run_give(p);
		return;
	}
	char *block = (char *)p - Y_MEM_HEAD;
	const size_t c = *(size_t *)block;
	*(void **)block = y_mem_free[c];
	y_mem_free[c] = block;
}

static void *y_mem_resize(void *p, size_t bytes) {
	if (!p) return y_mem_take(bytes);
	const size_t held = y_mem_held(p);
	if (bytes <= held) return p;
	void *moved = y_mem_take(bytes);
	if (!moved) return NULL;
	memcpy(moved, p, held);
	y_mem_give(p);
	return moved;
}

static void *y_mem_aligned(size_t align, size_t bytes) {
	if (align <= Y_MEM_HEAD) return y_mem_take(bytes);
	if (align > Y_MEM_PAGE) return NULL;
	return y_mem_run_take((bytes + Y_MEM_PAGE - 1) / Y_MEM_PAGE);
}

#ifndef Y_MEMORY_TEST
void *malloc(size_t bytes) { return y_mem_take(bytes); }
void free(void *p) { y_mem_give(p); }
void *realloc(void *p, size_t bytes) { return y_mem_resize(p, bytes); }
void *calloc(size_t count, size_t size) {
	if (size && count > SIZE_MAX / size) return NULL;
	void *p = y_mem_take(count * size);
	if (p) memset(p, 0, count * size);
	return p;
}
void *aligned_alloc(size_t align, size_t bytes) { return y_mem_aligned(align, bytes); }
int posix_memalign(void **out, size_t align, size_t bytes) {
	void *p = y_mem_aligned(align, bytes);
	if (!p) return 12;
	*out = p;
	return 0;
}
#endif
#endif
// ---- (end of a component's memory)

// memory the runtime keeps for itself (never collected)
void *y_alloc(size_t bytes) {
	void *p = malloc(bytes ? bytes : 1);
	if (!p) y_die("out of memory");
	return p;
}

y_frame *y_top;

const uint32_t y_class_size[Y_NCLASS] = { 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256,
	320, 384, 448, 512, 640, 768, 1024, 1536, 2048, 3072, 4096, 8192 };
uint8_t y_class_of[8192 / 16 + 1];
y_page *y_pages[Y_NCLASS], *y_tails[Y_NCLASS], *y_cursor[Y_NCLASS], *y_larges;
// a collection runs once the heap has doubled (tripled or quadrupled: see y_collect) since the last
// one left it, and never below
// y_heap_min (YEL_GC_MIN_MB, default 4): a program that keeps most of what it makes (a compile)
// runs faster with more (fewer collections), a component stays smaller with less
size_t y_heap_bytes, y_heap_min = (size_t)4 << 20, y_heap_limit = (size_t)4 << 20;
long y_stress, y_stress_left, y_collections, y_verbose;
// while it is not 0 nothing is collected: the JSON decoder builds a value no root holds yet
long y_gc_hold;
size_t y_peak_bytes;
void **y_roots[16];
int y_nroots;

void y_root(void **p) {
	if (y_nroots == (int)(sizeof y_roots / sizeof y_roots[0])) y_die("too many runtime roots");
	y_roots[y_nroots++] = p;
}

// the runtime's own C tables that name heap objects (the timers, the host's waits): each marks
// what it names when the collector runs, so nothing they name is freed under them
void (*y_tracers[8])(void);
int y_ntracers;

void y_tracer(void (*trace)(void)) {
	if (y_ntracers == (int)(sizeof y_tracers / sizeof y_tracers[0])) y_die("too many runtime tracers");
	y_tracers[y_ntracers++] = trace;
}

bool y_globals_made;

/** The program's globals made (each its literal, or its zero value: its collections made empty),
 * once, before its first code runs (main's, or the first export's), and marked from then on. */
void y_globals(void (*init)(void), void (*mark)(void)) {
	if (y_globals_made) return;
	y_globals_made = true;
	y_tracer(mark);
	init();
}

// ---- exported resources: the objects the host holds handles to, each resource's in a table by
// rep (its index: the handle's, natively; the host's handle names it in a component)

typedef struct {
	void **objects;
	int32_t len, cap;
	int32_t *free;
	int32_t nfree, free_cap;
} y_res_table;

static y_res_table *y_res_tables;
static int32_t y_nres_tables;

// every table's objects: the collector's roots (a host holding a handle keeps its object)
static void y_res_mark(void) {
	for (int32_t t = 0; t < y_nres_tables; t++)
		for (int32_t i = 0; i < y_res_tables[t].len; i++) y_mark(y_res_tables[t].objects[i]);
}

static y_res_table *y_res_at(int32_t table) {
	if (table >= y_nres_tables) {
		if (y_nres_tables == 0) y_tracer(y_res_mark);
		y_res_tables = realloc(y_res_tables, sizeof(y_res_table) * (size_t)(table + 1));
		memset(y_res_tables + y_nres_tables, 0, sizeof(y_res_table) * (size_t)(table + 1 - y_nres_tables));
		y_nres_tables = table + 1;
	}
	return &y_res_tables[table];
}

/** An object given the host (an own handle made): its rep, a free slot of its resource's table. */
int32_t y_res_new(int32_t table, void *object) {
	y_res_table *t = y_res_at(table);
	if (t->nfree > 0) {
		int32_t rep = t->free[--t->nfree];
		t->objects[rep] = object;
		return rep;
	}
	if (t->len == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 8;
		t->objects = realloc(t->objects, sizeof(void *) * (size_t)t->cap);
	}
	t->objects[t->len] = object;
	return t->len++;
}

/** The object a rep names (a borrowed handle's, a method's self). */
void *y_res_get(int32_t table, int32_t rep) {
	y_res_table *t = y_res_at(table);
	if (rep < 0 || rep >= t->len || !t->objects[rep]) y_die("a resource's handle the component did not give");
	return t->objects[rep];
}

/** A rep let go (the host dropped its handle): its slot free again, its object no longer held. */
void y_res_free(int32_t table, int32_t rep) {
	y_res_table *t = y_res_at(table);
	if (rep < 0 || rep >= t->len || !t->objects[rep]) return;
	t->objects[rep] = NULL;
	if (t->nfree == t->free_cap) {
		t->free_cap = t->free_cap ? t->free_cap * 2 : 8;
		t->free = realloc(t->free, sizeof(int32_t) * (size_t)t->free_cap);
	}
	t->free[t->nfree++] = rep;
}

y_pte *y_pt;
size_t y_pt_cap, y_pt_len;
// the page last looked up: pointers marked one after another are mostly in one page
uintptr_t y_pt_last_key = 1;
y_page *y_pt_last;

size_t y_pt_index(uintptr_t key, size_t cap) {
	size_t i = (size_t)((key >> Y_PAGE_SHIFT) * 0x9E3779B97F4A7C15ull) & (cap - 1);
	while (y_pt[i].key && y_pt[i].key != key) i = (i + 1) & (cap - 1);
	return i;
}

void y_pt_put(uintptr_t key, y_page *page) {
	if ((y_pt_len + 1) * 2 > y_pt_cap) {
		y_pte *old = y_pt;
		const size_t cap = y_pt_cap;
		y_pt_cap = cap ? cap * 2 : 1024;
		y_pt = calloc(y_pt_cap, sizeof(y_pte));
		if (!y_pt) y_die("out of memory");
		for (size_t i = 0; i < cap; i++) if (old[i].key) y_pt[y_pt_index(old[i].key, y_pt_cap)] = old[i];
		free(old);
	}
	const size_t i = y_pt_index(key, y_pt_cap);
	if (!y_pt[i].key) y_pt_len++;
	y_pt[i] = (y_pte){ key, page };
	y_pt_last_key = 1;
}

y_page *y_page_at(const void *p) {
	if (!y_pt) return NULL;
	const uintptr_t key = (uintptr_t)p & ~(uintptr_t)(Y_PAGE - 1);
	if (key == y_pt_last_key) return y_pt_last;
	y_pt_last_key = key;
	return y_pt_last = y_pt[y_pt_index(key, y_pt_cap)].page;
}

// the object p points into (its header) and its index in its page, or NULL
y_head *y_object(const void *p, y_page **page, size_t *index) {
	y_page *pg = y_page_at(p);
	if (!pg) return NULL;
	const char *c = p;
	if (pg->cls == Y_LARGE) {
		if (c < pg->first || c >= pg->first + pg->bytes) return NULL;
		*page = pg;
		*index = 0;
		return (y_head *)pg->first;
	}
	if (c < pg->first || c >= pg->bump) return NULL;
	const size_t i = (size_t)(c - pg->first) / pg->size;
	if (!(pg->live[i >> 6] >> (i & 63) & 1)) return NULL;
	*page = pg;
	*index = i;
	return (y_head *)(pg->first + i * pg->size);
}

// ---- marking

y_head **y_stack;
size_t y_stack_len, y_stack_cap;

/** Marks the object p points into (if any) live, and queues it for its trace. */
void y_mark(const void *p) {
	if (!p) return;
	y_page *pg;
	size_t i;
	y_head *h = y_object(p, &pg, &i);
	if (!h) return;
	const uint64_t bit = 1ull << (i & 63);
	if (pg->marks[i >> 6] & bit) return;
	pg->marks[i >> 6] |= bit;
	if (!h->trace) return;
	if (y_stack_len == y_stack_cap) {
		y_stack_cap = y_stack_cap ? y_stack_cap * 2 : 1024;
		y_stack = realloc(y_stack, y_stack_cap * sizeof(y_head *));
		if (!y_stack) y_die("out of memory");
	}
	y_stack[y_stack_len++] = h;
}

// a list's items and a map's values are marked through these: a pointer, or a string's bytes
void y_scan_ptr(void *value) { y_mark(*(void **)value); }

void y_scan_str(void *value) { y_mark(((ystr *)value)->data); }

void y_collect(void) {
	y_collections++;
	if (y_heap_bytes > y_peak_bytes) y_peak_bytes = y_heap_bytes;
	for (y_frame *f = y_top; f; f = f->prev) {
		for (int64_t i = 0; i < f->n; i++) y_mark(f->slots[i]);
		for (int64_t i = 0; i < f->m; i++) f->objs[i].trace(f->objs[i].obj);
	}
	for (int i = 0; i < y_nroots; i++) y_mark(*y_roots[i]);
	for (int i = 0; i < y_ntracers; i++) y_tracers[i]();
	while (y_stack_len) {
		y_head *h = y_stack[--y_stack_len];
		h->trace(h + 1);
	}
	// sweep: an object that is live and not marked is free again
	size_t live = 0;
	for (int c = 0; c < Y_NCLASS; c++) {
		for (y_page *pg = y_pages[c]; pg; pg = pg->next) {
			const size_t n = (size_t)(pg->bump - pg->first) / pg->size;
			for (size_t w = 0; w * 64 < n; w++) {
				uint64_t dead = pg->live[w] & ~pg->marks[w];
				while (dead) {
					char *o = pg->first + (w * 64 + (size_t)__builtin_ctzll(dead)) * pg->size;
					dead &= dead - 1;
					if (y_stress) memset(o, 0xAB, pg->size);
					*(void **)o = pg->free;
					pg->free = o;
					pg->used--;
				}
				pg->live[w] &= pg->marks[w];
				pg->marks[w] = 0;
			}
			live += (size_t)pg->used * pg->size;
		}
		y_cursor[c] = y_pages[c];
	}
	for (y_page **link = &y_larges; *link;) {
		y_page *pg = *link;
		if (pg->marks[0] & 1) {
			pg->marks[0] = 0;
			live += pg->bytes;
			link = &pg->next;
			continue;
		}
		*link = pg->next;
		for (size_t off = 0; off < pg->bytes; off += Y_PAGE) y_pt_put((uintptr_t)(pg->first + off), NULL);
		if (y_stress) memset(pg->first, 0xAB, pg->bytes);
		free(pg->first);
		free(pg);
	}
	// the next collection when the heap has doubled; tripled, once more than 16 MiB is live (each
	// collection marks all of it: a large live heap, as a compile's, is marked less often for a
	// third more memory); or quadrupled, when this one freed less than a quarter of it (a heap that
	// is mostly live, growing, gains little from being marked often). A small program, a
	// component's turns, stays at double
	const size_t grow = live * 4 > y_heap_bytes * 3 ? 4 : live > ((size_t)16 << 20) ? 3 : 2;
#if !defined(Y_HOSTED)
	if (y_verbose)
		fprintf(stderr, "yel gc: %.1f MiB -> %.1f MiB live, next at x%d\n", (double)y_heap_bytes / 1048576.0,
			(double)live / 1048576.0, (int)grow);
#endif
	y_heap_bytes = live;
	y_heap_limit = live * grow > y_heap_min ? live * grow : y_heap_min;
}

// ---- allocating

void *y_page_take(y_page *pg) {
	char *o;
	if (pg->free) {
		o = pg->free;
		pg->free = *(void **)o;
	} else if (pg->bump + pg->size <= pg->end) {
		o = pg->bump;
		pg->bump += pg->size;
	} else return NULL;
	const size_t i = (size_t)(o - pg->first) / pg->size;
	pg->live[i >> 6] |= 1ull << (i & 63);
	pg->used++;
	return o;
}

void *y_pages_alloc(size_t bytes) {
	void *p = aligned_alloc(Y_PAGE, bytes);
	if (!p) y_die("out of memory");
	return p;
}

y_page *y_page_new(int c) {
	char *mem = y_pages_alloc(Y_PAGE);
	y_page *pg = (y_page *)mem;
	memset(pg, 0, sizeof *pg);
	pg->cls = (uint32_t)c;
	pg->size = y_class_size[c];
	pg->first = mem + ((sizeof(y_page) + 15) & ~(size_t)15);
	pg->end = mem + Y_PAGE;
	pg->bump = pg->first;
	if (y_tails[c]) y_tails[c]->next = pg;
	else y_pages[c] = pg;
	y_tails[c] = pg;
	y_pt_put((uintptr_t)mem, pg);
	return pg;
}

y_head *y_new_large(size_t need) {
	if (need > SIZE_MAX - Y_PAGE) y_die("out of memory");
	const size_t bytes = (need + Y_PAGE - 1) & ~(Y_PAGE - 1);
	y_page *pg = calloc(1, sizeof *pg);
	if (!pg) y_die("out of memory");
	pg->cls = Y_LARGE;
	pg->first = y_pages_alloc(bytes);
	pg->bytes = bytes;
	pg->next = y_larges;
	y_larges = pg;
	for (size_t off = 0; off < bytes; off += Y_PAGE) y_pt_put((uintptr_t)(pg->first + off), pg);
	y_heap_bytes += bytes;
	return (y_head *)pg->first;
}

/** A new heap object of bytes (zeroed), its pointers marked by trace (NULL: it holds none). */
void *y_new(size_t bytes, y_trace trace) { return y_new_block(bytes, trace, true); }

/** New heap bytes, not zeroed: for what the caller fills at once (a string's bytes, a list's items
 *  up to its length). */
void *y_new_bytes(size_t bytes) { return y_new_block(bytes, NULL, false); }

void *y_new_block(size_t bytes, y_trace trace, bool zero) {
	const size_t need = bytes + sizeof(y_head);
#ifndef Y_NO_GC
	if (!y_turns && !y_gc_hold && (y_stress ? --y_stress_left <= 0 : y_heap_bytes >= y_heap_limit)) {
		y_stress_left = y_stress;
		y_collect();
	}
#endif
	y_head *h;
	if (need > y_class_size[Y_NCLASS - 1]) h = y_new_large(need);
	else {
		if (!y_class_of[0]) {
			for (int c = 0, k = 0; k <= 8192 / 16; k++) {
				while (y_class_size[c] < (uint32_t)k * 16) c++;
				y_class_of[k] = (uint8_t)c;
			}
			y_class_of[0] = 1;
		}
		const int c = y_class_of[(need + 15) / 16];
		y_page *pg = y_cursor[c];
		void *o = NULL;
		while (pg && !(o = y_page_take(pg))) pg = pg->next;
		if (!o) {
			pg = y_page_new(c);
			o = y_page_take(pg);
		}
		y_cursor[c] = pg;
		y_heap_bytes += pg->size;
		h = o;
	}
	if (zero) memset(h + 1, 0, bytes);
	h->trace = trace;
	return h + 1;
}

// ---------------------------------------------------------------- strings

ystr y_str_of(const char *bytes, int64_t len) {
	char *d = y_new_bytes((size_t)len + 1);
	memcpy(d, bytes, (size_t)len);
	d[len] = 0;
	return (ystr){ len, d };
}

// ---------------------------------------------------------------- floats as text, exactly

// a float's text without the C library's printf and strtod (a guest's build is smaller without
// them): its exact decimal digits (a big integer's), rounded as %.*g and %.*f round them (to
// nearest, ties to even, on the exact value), and whether a text reads back as the float (it lies
// in the float's rounding interval, ties to the even mantissa: strtod's own rounding)

// an unsigned big integer: 32-bit words, least first (a float's exact value needs 2600 bits at most,
// a comparison of one with a decimal 3800)
typedef struct {
	uint32_t w[160];
	int n;
} y_big;

static void y_big_set(y_big *b, uint64_t v) {
	b->n = 0;
	while (v) {
		b->w[b->n++] = (uint32_t)v;
		v >>= 32;
	}
}

static void y_big_mul_small(y_big *b, uint32_t m) {
	uint64_t carry = 0;
	for (int i = 0; i < b->n; i++) {
		const uint64_t t = (uint64_t)b->w[i] * m + carry;
		b->w[i] = (uint32_t)t;
		carry = t >> 32;
	}
	if (carry) b->w[b->n++] = (uint32_t)carry;
}

static void y_big_mul_pow5(y_big *b, int k) {
	static const uint32_t small[13] = { 1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125, 9765625, 48828125, 244140625 };
	while (k >= 13) {
		y_big_mul_small(b, 1220703125u);
		k -= 13;
	}
	if (k > 0) y_big_mul_small(b, small[k]);
}

static void y_big_shl(y_big *b, int k) {
	if (b->n == 0 || k == 0) return;
	const int words = k / 32, bits = k % 32;
	if (bits) {
		uint32_t carry = 0;
		for (int i = 0; i < b->n; i++) {
			const uint32_t next = b->w[i] >> (32 - bits);
			b->w[i] = (b->w[i] << bits) | carry;
			carry = next;
		}
		if (carry) b->w[b->n++] = carry;
	}
	if (words) {
		for (int i = b->n - 1; i >= 0; i--) b->w[i + words] = b->w[i];
		for (int i = 0; i < words; i++) b->w[i] = 0;
		b->n += words;
	}
}

static int y_big_cmp(const y_big *a, const y_big *b) {
	if (a->n != b->n) return a->n < b->n ? -1 : 1;
	for (int i = a->n - 1; i >= 0; i--) {
		if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
	}
	return 0;
}

static uint32_t y_big_divmod_small(y_big *b, uint32_t d) {
	uint64_t rest = 0;
	for (int i = b->n - 1; i >= 0; i--) {
		const uint64_t cur = (rest << 32) | b->w[i];
		b->w[i] = (uint32_t)(cur / d);
		rest = cur % d;
	}
	while (b->n > 0 && b->w[b->n - 1] == 0) b->n--;
	return (uint32_t)rest;
}

// a positive finite float as m * 2^e (m its mantissa, the implied bit set where it is normal),
// single or double; whether the gap below it is half the one above (a power of two, past the least)
static void y_float_parts(double v, bool single, uint64_t *m, int *e, bool *narrow) {
	if (single) {
		uint32_t bits;
		const float f = (float)v;
		memcpy(&bits, &f, sizeof bits);
		const uint32_t fraction = bits & 0x7fffff;
		const int biased = (int)((bits >> 23) & 0xff);
		*m = biased ? (fraction | 0x800000) : fraction;
		*e = biased ? biased - 150 : -149;
		*narrow = biased > 1 && fraction == 0;
	} else {
		uint64_t bits;
		memcpy(&bits, &v, sizeof bits);
		const uint64_t fraction = bits & ((1ull << 52) - 1);
		const int biased = (int)((bits >> 52) & 0x7ff);
		*m = biased ? (fraction | (1ull << 52)) : fraction;
		*e = biased ? biased - 1075 : -1074;
		*narrow = biased > 1 && fraction == 0;
	}
}

// a positive finite float's exact decimal digits (into out, no leading or trailing zeros past the
// first), how many, and the power of ten of the first (v = d.ddd × 10^x)
static int y_float_digits(double v, char *out, int *x) {
	uint64_t m;
	int e;
	bool narrow;
	y_float_parts(v, false, &m, &e, &narrow);
	y_big b;
	y_big_set(&b, m);
	int scale = 0;
	if (e >= 0) y_big_shl(&b, e);
	else {
		y_big_mul_pow5(&b, -e);
		scale = -e;
	}
	char reversed[900];
	int n = 0;
	while (b.n > 0) {
		uint32_t chunk = y_big_divmod_small(&b, 1000000000u);
		for (int k = 0; k < 9; k++) {
			reversed[n++] = (char)('0' + chunk % 10);
			chunk /= 10;
		}
	}
	while (n > 1 && reversed[n - 1] == '0') n--;
	for (int k = 0; k < n; k++) out[k] = reversed[n - 1 - k];
	*x = n - 1 - scale;
	int kept = n;
	while (kept > 1 && out[kept - 1] == '0') kept--;
	return kept;
}

// digits (n of them, the first's power of ten x) rounded to keep (≥ 1) digits, ties to even: into
// out (keep of them, zeros past the last), the power of ten told again (a carry: one more)
static void y_round_digits(const char *digits, int n, int keep, char *out, int *x) {
	for (int k = 0; k < keep; k++) out[k] = k < n ? digits[k] : '0';
	if (n <= keep) return;
	bool up = digits[keep] > '5';
	if (digits[keep] == '5') {
		bool past = false;
		for (int k = keep + 1; k < n; k++) past = past || digits[k] != '0';
		up = past || ((out[keep - 1] - '0') & 1);
	}
	if (!up) return;
	int k = keep - 1;
	while (k >= 0 && out[k] == '9') out[k--] = '0';
	if (k >= 0) out[k]++;
	else {
		out[0] = '1';
		*x += 1;
	}
}

// r × 10^q against m × 2^f: -1, 0 or 1
static int y_compare_scaled(uint64_t r, int q, uint64_t m, int f) {
	y_big a, b;
	y_big_set(&a, r);
	y_big_set(&b, m);
	if (q >= 0) y_big_mul_pow5(&a, q);
	else y_big_mul_pow5(&b, -q);
	const int shift = q - f;
	if (shift >= 0) y_big_shl(&a, shift);
	else y_big_shl(&b, -shift);
	return y_big_cmp(&a, &b);
}

// whether the decimal d (keep digits, the first's power of ten x) reads back as v (positive,
// finite: single or double), as strtod and strtof round
static bool y_reads_back(double v, bool single, const char *d, int keep, int x) {
	uint64_t r = 0;
	for (int k = 0; k < keep; k++) r = r * 10 + (uint64_t)(d[k] - '0');
	const int q = x - (keep - 1);
	uint64_t m;
	int e;
	bool narrow;
	y_float_parts(v, single, &m, &e, &narrow);
	const bool odd = m & 1;
	// (above: halfway to the next float; below: halfway to the one before, a quarter gap at a power
	// of two)
	const int above = y_compare_scaled(r, q, 2 * m + 1, e - 1);
	if (above > 0 || (above == 0 && odd)) return false;
	const int below = narrow ? y_compare_scaled(r, q, 4 * m - 1, e - 2) : y_compare_scaled(r, q, 2 * m - 1, e - 1);
	return !(below < 0 || (below == 0 && odd));
}

// %.*g's text of digits (keep of them, the first's power of ten x): its trailing zeros dropped
static int y_g_text(char *out, bool negative, const char *d, int keep, int x, int precision) {
	int len = 0;
	if (negative) out[len++] = '-';
	int last = keep;
	while (last > 1 && d[last - 1] == '0') last--;
	if (x < -4 || x >= precision) {
		out[len++] = d[0];
		if (last > 1) {
			out[len++] = '.';
			for (int k = 1; k < last; k++) out[len++] = d[k];
		}
		out[len++] = 'e';
		out[len++] = x < 0 ? '-' : '+';
		const int magnitude = x < 0 ? -x : x;
		if (magnitude >= 100) out[len++] = (char)('0' + magnitude / 100);
		out[len++] = (char)('0' + magnitude / 10 % 10);
		out[len++] = (char)('0' + magnitude % 10);
	} else if (x >= 0) {
		for (int k = 0; k <= x; k++) out[len++] = k < last ? d[k] : '0';
		if (last > x + 1) {
			out[len++] = '.';
			for (int k = x + 1; k < last; k++) out[len++] = d[k];
		}
	} else {
		out[len++] = '0';
		out[len++] = '.';
		for (int k = 0; k < -x - 1; k++) out[len++] = '0';
		for (int k = 0; k < last; k++) out[len++] = d[k];
	}
	return len;
}

// a float's text, the shortest digits that read back as it (single: as a float), up to most, laid
// out as %.*g lays out most digits (10, not 1e+01: exponents from most places on, or below 10^-4)
static ystr y_shortest_text(double v, bool single, int most) {
	char out[48];
	const bool negative = signbit(v);
	if (v == 0) return negative ? y_str_of("-0", 2) : y_str_of("0", 1);
	const double size = negative ? -v : v;
	char digits[900];
	int x;
	const int n = y_float_digits(size, digits, &x);
	char rounded[24];
	int len = 0;
	for (int precision = 1; precision <= most; precision++) {
		int rx = x;
		y_round_digits(digits, n, precision, rounded, &rx);
		len = y_g_text(out, negative, rounded, precision, rx, most);
		if (y_reads_back(size, single, rounded, precision, rx)) break;
	}
	return y_str_of(out, len);
}

// %.*f's text: places digits after the point, the last rounded (ties to even)
static ystr y_fixed_text(double v, int places) {
	const bool negative = signbit(v);
	const double size = negative ? -v : v;
	char digits[900];
	int x = 0;
	int n = 1;
	if (size == 0) digits[0] = '0';
	else n = y_float_digits(size, digits, &x);
	// (the digits kept: those down to the places' last; none kept: 0, or 1 there where it rounds up)
	const int keep = x + 1 + places;
	char *out = malloc((size_t)(keep > 0 ? keep : 1) + (size_t)places + 8);
	if (!out) y_die("out of memory");
	char *kept = malloc((size_t)(keep > 0 ? keep : 1) + 2);
	if (!kept) y_die("out of memory");
	int kx = x;
	int count;
	if (keep > 0) {
		y_round_digits(digits, n, keep, kept, &kx);
		count = keep + (kx - x);
		if (kx != x) kept[keep] = '0';
	} else {
		// (|v| < 10^-places: 10^-places where it is past halfway, ties to even: 0 is even)
		bool up = false;
		if (keep == 0 && size != 0) {
			bool past = false;
			for (int k = 1; k < n; k++) past = past || digits[k] != '0';
			up = digits[0] > '5' || (digits[0] == '5' && past);
		}
		kept[0] = up ? '1' : '0';
		count = 1;
		kx = up ? -places : -places;
	}
	// kept: count digits, the first's power of ten kx (kept[k] is 10^(kx - k))
	int len = 0;
	if (negative) out[len++] = '-';
	if (kx < 0) out[len++] = '0';
	else {
		for (int k = 0; k <= kx; k++) out[len++] = k < count ? kept[k] : '0';
	}
	if (places > 0) {
		out[len++] = '.';
		for (int p = 1; p <= places; p++) {
			const int k = kx + p;
			out[len++] = k >= 0 && k < count ? kept[k] : '0';
		}
	}
	ystr text = y_str_of(out, len);
	free(kept);
	free(out);
	return text;
}

// a float's text, the shortest that reads back as it (nan, inf, -inf): std's format.write-f64
ystr yel_f64_text(double v) {
	if (isnan(v)) return y_str_of("nan", 3);
	if (isinf(v)) return v > 0 ? y_str_of("inf", 3) : y_str_of("-inf", 4);
	return y_shortest_text(v, false, 17);
}

ystr yel_f32_text(float v) {
	if (isnan(v) || isinf(v)) return yel_f64_text(v);
	return y_shortest_text(v, true, 9);
}

void y_trace_buffer(void *obj) { y_mark(((ybuffer *)obj)->data); }

ybuffer *yel_buffer_new(int64_t cap) {
	Y_FRAME(1);
	ybuffer *b = y_new(sizeof(ybuffer), y_trace_buffer);
	// rooted while its bytes are made
	ys_[0] = b;
	b->len = 0;
	b->cap = 0;
	b->data = NULL;
	if (cap > 0) {
		b->data = y_new_bytes((size_t)cap);
		b->cap = cap;
	}
	Y_POP();
	return b;
}

// the bytes so far as a string: shared, not copied
ystr yel_buffer_string(ybuffer *b) { return b->len ? (ystr){ b->len, b->data } : (ystr){ 0, "" }; }

// whether stderr may be colored: a terminal, and NO_COLOR not set
bool yel_stderr_color(void) {
#if defined(Y_HOSTED)
	// (a host's stream: no terminal it knows of)
	return false;
#else
	const char *no = y_getenv("NO_COLOR");
	return (no == NULL || no[0] == 0) && isatty(2);
#endif
}

// a match no arm fit: the program stops, showing the value
Y_NORETURN void yel_no_match(ystr shown) {
	y_out_flush();
	y_out_text(2, "yel: no match arm for ");
	(void)y_out(2, shown.data, (size_t)shown.len);
	y_out_text(2, "\n");
	y_stop();
}

// ---------------------------------------------------------------- lists

void y_trace_list(void *obj) {
	ylist *l = obj;
	y_mark(l->items);
	if (l->scan)
		for (int64_t i = 0; i < l->len; i++) l->scan(l->items + i * l->size);
}

/** The bytes of cap items of size bytes (a size no address space holds: out of memory). */
size_t y_list_bytes(int64_t size, int64_t cap) {
	if (cap < 0 || size < 0 || (size > 0 && (uint64_t)cap > (uint64_t)(SIZE_MAX / 2) / (uint64_t)size)) {
		y_die("out of memory (a list too large)");
	}
	return (size_t)(size * cap);
}

/** An empty list of items of size bytes (scan marks one; NULL when they hold no pointers). One
 *  block: the list, then room for its first cap items (at least 4; a byte at least, so items points
 *  inside it). Grown past them, its items move to a block of their own (std's list.grow); the
 *  collector marks items either way (an interior pointer marks the block it is in). */
ylist *y_list_new(int64_t cap, int64_t size, y_scan scan) {
	if (cap < 4) cap = 4;
	const size_t bytes = y_list_bytes(size, cap);
	ylist *l = y_new_block(sizeof(ylist) + (bytes ? bytes : 1), y_trace_list, false);
	l->len = 0;
	l->cap = cap;
	l->size = size;
	l->scan = scan;
	l->items = (char *)(l + 1);
	return l;
}

/** A list literal: its n items. */
ylist *y_list_of(int64_t n, int64_t size, const void *items, y_scan scan) {
	ylist *l = y_list_new(n, size, scan);
	memcpy(l->items, items, (size_t)(size * n));
	l->len = n;
	return l;
}

// a list's static data (a literal of constants, the compiler's) made a list of the program's: its
// items copied, and every list in them, by its type's descriptor (strings stay where they are:
// static, and never written)
static ylist *y_static_copy(const ylist *data, const ytype *t);

static void y_static_fresh(char *at, const ytype *t) {
	switch (t->kind) {
	case Y_K_LIST: {
		ylist **held = (ylist **)at;
		if (*held) *held = y_static_copy(*held, t);
		break;
	}
	case Y_K_TUPLE:
	case Y_K_ANON_RECORD:
	case Y_K_VALUE_RECORD:
		for (uint32_t k = 0; k < t->count; k++) y_static_fresh(at + t->offsets[k], t->parts[k]);
		break;
	default:
		break;
	}
}

// whether a value of type t holds a list (its items then made fresh one by one)
static bool y_static_holds_list(const ytype *t) {
	if (t->kind == Y_K_LIST) return true;
	if (t->kind == Y_K_TUPLE || t->kind == Y_K_ANON_RECORD || t->kind == Y_K_VALUE_RECORD) {
		for (uint32_t k = 0; k < t->count; k++) if (y_static_holds_list(t->parts[k])) return true;
	}
	return false;
}

static ylist *y_static_copy(const ylist *data, const ytype *t) {
	ylist *l = y_list_of(data->len, data->size, data->items, data->scan);
	const ytype *item = t->parts[0];
	if (y_static_holds_list(item)) {
		for (int64_t i = 0; i < l->len; i++) y_static_fresh(l->items + i * l->size, item);
	}
	return l;
}

/** A list literal of constants holding no list: its static items (n, each size bytes) copied. */
ylist *y_list_of_data(int64_t n, int64_t size, const void *items, y_scan scan) { return y_list_of(n, size, items, scan); }

/** A list literal of constants: its static data (data, of type t: a list's descriptor) copied, each
list in it too, the collector held while what is made is held by nothing else. */
ylist *y_list_static(const ylist *data, const ytype *t) {
	y_gc_hold++;
	ylist *l = y_static_copy(data, t);
	y_gc_hold--;
	return l;
}

void *y_list_push_slot(ylist *l) {
	if (l->len == l->cap) {
		char *next = y_new_bytes(y_list_bytes(l->size, l->cap * 2));
		memcpy(next, l->items, (size_t)(l->size * l->len));
		l->items = next;
		l->cap *= 2;
	}
	return l->items + l->len++ * l->size;
}

// ---------------------------------------------------------------- maps

// zeroed bytes of the heap (a map's index, hashes, keys and values start empty)
void *y_new_zeroed(size_t bytes) { return y_new(bytes, NULL); }

// a hash of a value's bits (a number, a char, a pointer: what == compares)
uint64_t y_hash_bits(uint64_t v) {
	v ^= v >> 33;
	v *= 0xff51afd7ed558ccdull;
	v ^= v >> 33;
	return v | 1;
}

// two hashes as one (a tuple's, a record's)
uint64_t y_hash_mix(uint64_t h, uint64_t v) { return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))) | 1; }

uint64_t y_hash_address(const void *p) { return y_hash_bits((uint64_t)(uintptr_t)p); }

uint64_t y_hash_f32(float f) { return y_hash_bits(f == 0 ? 0 : y_bits_f32(f)); }

uint64_t y_hash_f64(double f) { return y_hash_bits(f == 0 ? 0 : y_bits_f64(f)); }

void y_trace_map(void *obj) {
	ymap *m = obj;
	y_mark(m->index);
	y_mark(m->hashes);
	y_mark(m->keys);
	y_mark(m->values);
	// a map still being made (a collection in y_map_new) has no entries yet
	if (!m->hashes || !m->keys || !m->values) return;
	for (int64_t i = 0; i < m->used; i++) {
		if (!m->hashes[i]) continue;
		if (m->kscan) m->kscan(m->keys + i * m->ksize);
		if (m->vscan) m->vscan(m->values + i * m->vsize);
	}
}

/** An empty map with room for capacity entries (its index for twice as many): keys of ksize bytes
 *  (kscan marks one; hash and eq: its type's), values of vsize. Its table is std's (map.yel). */
ymap *y_map_make(int64_t ksize, y_scan kscan, uint64_t (*hash)(const void *), bool (*eq)(const void *, const void *),
	int64_t vsize, y_scan vscan, int64_t capacity) {
	Y_FRAME(1);
	ymap *m = y_new(sizeof(ymap), y_trace_map);
	ys_[0] = m;
	m->ksize = ksize;
	m->vsize = vsize;
	m->kscan = kscan;
	m->vscan = vscan;
	m->hash = hash;
	m->eq = eq;
	m->cap = capacity;
	m->icap = capacity * 2;
	m->index = y_new_zeroed((size_t)m->icap * sizeof(int64_t));
	m->hashes = y_new_zeroed((size_t)m->cap * sizeof(uint64_t));
	m->keys = y_new_zeroed(y_list_bytes(ksize, m->cap));
	m->values = y_new_zeroed(y_list_bytes(vsize, m->cap));
	Y_POP();
	return m;
}

// the prelude's generic map funcs: for each type argument its C type and what scans one first,
// and a key's (K) its hash and equality too. A key is given by its address (a copy of it)
// a set: a map whose values are ()

// ---------------------------------------------------------------- strings: bytes

// a float with digits after the point, rounded (std's format.fixed): nan, inf and -inf as
// interpolation shows them
ystr yel_fixed(double value, int32_t digits) {
	if (isnan(value)) return y_str_of("nan", 3);
	if (isinf(value)) return value > 0 ? y_str_of("inf", 3) : y_str_of("-inf", 4);
	if (digits < 0) digits = 0;
	if (digits > 100) digits = 100;
	return y_fixed_text(value, digits);
}

// ---------------------------------------------------------------- the process

ylist *y_args;

// an integer's decimal digits written at out (ends it): where they end
static char *y_put_int(char *out, int64_t v) {
	char digits[24];
	int n = 0;
	uint64_t size = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
	do {
		digits[n++] = (char)('0' + size % 10);
		size /= 10;
	} while (size > 0);
	if (v < 0) *out++ = '-';
	while (n > 0) *out++ = digits[--n];
	*out = 0;
	return out;
}

Y_NORETURN void y_index_out_of_range(int64_t i, int64_t n) {
	char message[96];
	char *at = message;
	memcpy(at, "index ", 6);
	at = y_put_int(at + 6, i);
	memcpy(at, " out of range (length ", 22);
	at = y_put_int(at + 22, n);
	memcpy(at, ")", 2);
	y_die(message);
}

yunit yel_print(ystr s) {
	(void)y_out(1, s.data, (size_t)s.len);
	return 0;
}

yunit yel_eprint(ystr s) {
	(void)y_out(2, s.data, (size_t)s.len);
	return 0;
}

/** The target triple this runtime was built for (yelc's default for --backend bitcode; "" for one
 * it does not know). */
ystr yel_host_triple(void) {
#if defined(__wasm32__)
	const char *triple = "wasm32-unknown-wasip3";
#elif defined(__aarch64__) && defined(__APPLE__)
	const char *triple = "arm64-apple-macosx";
#elif defined(__aarch64__) && defined(__linux__)
	const char *triple = "aarch64-unknown-linux-gnu";
#elif defined(__x86_64__) && defined(__APPLE__)
	const char *triple = "x86_64-apple-macosx";
#elif defined(__x86_64__) && defined(__linux__)
	const char *triple = "x86_64-unknown-linux-gnu";
#else
	const char *triple = "";
#endif
	return (ystr){ (int64_t)strlen(triple), triple };
}

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <limits.h>
#include <stdlib.h>
#endif

/**
 * The running program's own file, every symlink followed (a link on PATH, Homebrew's in bin/),
 * found once; "" where the system does not say (WASI, another one).
 */
ystr yel_executable_path(void) {
#if defined(__APPLE__) || defined(__linux__)
	static char resolved[PATH_MAX];
	static int found = -1;

	if (found < 0) {
		found = 0;
#if defined(__APPLE__)
		char raw[PATH_MAX];
		uint32_t size = sizeof raw;

		if (_NSGetExecutablePath(raw, &size) == 0 && realpath(raw, resolved)) found = 1;
#else
		if (realpath("/proc/self/exe", resolved)) found = 1;
#endif
	}
	const char *path = found ? resolved : "";
#else
	const char *path = "";
#endif
	return (ystr){ (int64_t)strlen(path), path };
}

/** What main does last: stdout flushed, main's value the exit code. */
int y_finish(int64_t code) {
	y_out_flush();
	return (int)code;
}

Y_NORETURN yunit yel_panic(ystr message) {
	y_out_flush();
	y_out_text(2, "panic: ");
	yel_eprint(message);
	y_out_text(2, "\n");
	y_stop();
}

char *y_cstr(ystr s) {
	char *out = y_alloc((size_t)s.len + 1);
	memcpy(out, s.data, (size_t)s.len);
	out[s.len] = 0;
	return out;
}

/** A decimal float's value (as a float literal is written), for the compiler's keys. */
double yel_parse_f64(ystr s) {
	char *c = y_cstr(s);
	const double v = strtod(c, NULL);
	free(c);
	return v;
}

// YEL_GC_STATS: how the collector did, on stderr at exit
void y_stats(void) {
#if defined(Y_HOSTED)
	// (a guest's: none, so no printf)
	return;
#endif
	if (y_heap_bytes > y_peak_bytes) y_peak_bytes = y_heap_bytes;
	fprintf(stderr, "yel gc: %ld collections, heap %.1f MiB at exit, %.1f MiB at most\n", y_collections,
		(double)y_heap_bytes / 1048576.0, (double)y_peak_bytes / 1048576.0);
}

// the program's arguments as main has them (the native WASI host's get-arguments reads them)
int y_argc;
char **y_argv;

void **y_abi_temps;
size_t y_abi_ntemps, y_abi_captemps;

void *y_abi_temp(size_t bytes) {
	void *p = y_alloc(bytes);
	if (y_abi_ntemps == y_abi_captemps) {
		y_abi_captemps = y_abi_captemps ? y_abi_captemps * 2 : 16;
		y_abi_temps = realloc(y_abi_temps, y_abi_captemps * sizeof(void *));
		if (!y_abi_temps) y_die("out of memory");
	}
	y_abi_temps[y_abi_ntemps++] = p;
	return p;
}

void y_abi_free_temps(void) {
	for (size_t i = 0; i < y_abi_ntemps; i++) free(y_abi_temps[i]);
	y_abi_ntemps = 0;
}

void y_flush_stdout(void) { y_out_flush(); }

Y_NORETURN void y_abi_bad(int32_t what) {
	static const char *const said[] = { "a value outside its type crossed the boundary", "a case the union does not have crossed the boundary", "a case the variant does not have", "a case the enum does not have crossed the boundary", "a char that is not a Unicode scalar value crossed the boundary", "a tag out of its type's range crossed the boundary", "a future's writer was dropped before it wrote" };
	y_die(said[what < 0 || what > 6 ? 0 : what]);
}

// what cabi_realloc gives for no bytes (an empty string or list the host lowers): no memory taken
static max_align_t y_abi_nothing;

/** What the host wrote (len bytes or items, from cabi_realloc) freed, once lifted: an empty one's
 *  never (the host may not have asked for memory: its pointer is then anything: 1, say). */
void y_abi_free_host(void *ptr, size_t len) {
	if (len > 0 && ptr != (void *)&y_abi_nothing) free(ptr);
}

/** A string the host wrote: copied to the heap, the host's freed. */
ystr y_abi_lift_str(yabi_str *s) {
	ystr out = y_str_of(s->ptr, (int64_t)s->len);
	y_abi_free_host(s->ptr, s->len);
	return out;
}

#if defined(__wasm__)
// what the host writes into the component's memory (a string, a list) comes from here: plain
// memory, which lifting copies to the heap and frees
__attribute__((export_name("cabi_realloc"))) void *cabi_realloc(void *old, size_t old_size, size_t align, size_t new_size) {
	(void)old_size;
	(void)align;
	if (!old && new_size == 0) return &y_abi_nothing;
	void *p = realloc(old, new_size ? new_size : 1);
	if (!p) y_die("out of memory");
	return p;
}

#endif

// a component of exports only (no main): made ready by its first export's call
bool y_started;
void y_ready(void) {
	if (!y_started) y_start(0, NULL);
}

bool y_turns;
void y_ready_turns(void) {
	y_turns = true;
	y_ready();
}

// a turn's end (an export's post-return, an async export's call given back to the host: nothing
// of the component is on the stack): where y_turns, its only collections (see yel.h), once the
// heap has grown past what starts one (every turn under YEL_GC_STRESS); elsewhere one runs early,
// past half of it
void y_turn_end(void) {
#ifndef Y_NO_GC
	if (y_turns ? y_stress || y_heap_bytes >= y_heap_limit : !y_stress && y_heap_bytes >= y_heap_limit / 2) y_collect();
#endif
}

// a count from the environment: a decimal, 0 for anything else (no overflow: strtol clamps)
long y_env_count(const char *s) {
	char *end;
	const long n = strtol(s, &end, 10);
	return n < 0 ? 0 : n;
}

#if defined(__wasi__)
int y_main(int argc, char **argv, int (*program)(int argc, char **argv)) { return program(argc, argv); }
#else
#include <pthread.h>

typedef struct {
	int argc;
	char **argv;
	int (*program)(int argc, char **argv);
	int code;
} y_main_call;

static void *y_main_thread(void *given) {
	y_main_call *call = given;
	call->code = call->program(call->argc, call->argv);
	return NULL;
}

int y_main(int argc, char **argv, int (*program)(int argc, char **argv)) {
	const char *wanted = getenv("YEL_STACK_MB");
	long mb = wanted ? atol(wanted) : 1024;
	if (mb < 1) mb = 1024;
	y_main_call call = { argc, argv, program, 0 };
	pthread_attr_t attributes;
	pthread_t thread;
	// (where no thread of that stack can be made: main's own)
	if (pthread_attr_init(&attributes) != 0) return program(argc, argv);
	bool made = pthread_attr_setstacksize(&attributes, (size_t)mb << 20) == 0 && pthread_create(&thread, &attributes, y_main_thread, &call) == 0;
	pthread_attr_destroy(&attributes);
	if (!made) return program(argc, argv);
	pthread_join(thread, NULL);
	return call.code;
}
#endif

/** The program's arguments (argv after the program), for main. */
#if !defined(__wasi__)
#include <sys/resource.h>
#endif

ylist *y_start(int argc, char **argv) {
	y_started = true;
#if !defined(__wasi__)
	// as many files open at once as the system lets this process have (Linux gives 1024 unless
	// asked: a program with many tasks each holding one runs out)
	struct rlimit files;
	if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < files.rlim_max) {
		files.rlim_cur = files.rlim_max;
#if defined(__APPLE__)
		if (files.rlim_cur > OPEN_MAX) files.rlim_cur = OPEN_MAX;
#endif
		setrlimit(RLIMIT_NOFILE, &files);
	}
#endif
	y_argc = argc;
	y_argv = argv;
	const char *stress = y_getenv("YEL_GC_STRESS");
	if (stress) y_stress = y_stress_left = y_env_count(stress);
	const char *stats = y_getenv("YEL_GC_STATS");
	if (stats) {
		atexit(y_stats);
		// YEL_GC_STATS=2: each collection too
		y_verbose = y_env_count(stats) > 1;
	}
	const char *min = y_getenv("YEL_GC_MIN_MB");
	if (min) {
		const long mb = y_env_count(min);
		y_heap_min = y_heap_limit = (size_t)(mb > 1024 * 1024 ? 1024 * 1024 : mb) << 20;
	}
	y_root((void **)&y_args);
	y_args = y_list_new(argc, sizeof(ystr), y_scan_str);
	for (int i = 1; i < argc; i++) ((ystr *)y_args->items)[y_args->len++] = y_str_of(argv[i], (int64_t)strlen(argv[i]));
	return y_args;
}

// ================================================================ async (async.h)

int64_t y_async_order;
ylist *y_async_list;    // every started task (rooted)
ylist *y_async_ready;   // the tasks to step, in order (from y_async_head on)
int64_t y_async_head;
ylist *y_async_waits;   // pairs: a started task, a task waiting for it
y_async *y_async_now;   // the task being stepped: what a wait parks
y_timer *y_timers;
int64_t y_ntimers, y_timers_cap;

ylist *y_async_rooted(ylist **l) {
	if (!*l) {
		*l = y_list_new(0, sizeof(void *), y_scan_ptr);
		y_root((void **)l);
	}
	return *l;
}

ylist *yel_async_tasks(void) { return y_async_rooted(&y_async_list); }

// ---- YEL_ASYNC_TRACE: what the executor does with tasks, told on stderr. 1: each task started
// (by which), each time it parks and what it waits for, and once it is done (how long it was
// stepped, in how many steps, how long it lived); the tasks still waiting at a deadlock, and those
// never done at the program's end. 2: each step and each wake too. Unset (or 0): nothing told,
// nothing kept

// (waits: what it waits for, kept where it parks in this step; last: what it waited for last)
typedef struct { int64_t started, stepped, steps; char waits[120]; char last[120]; } y_task_trace;
static int y_trace_level = -1;
static y_task_trace *y_traces;
static int64_t y_ntraces, y_traces_cap, y_task_count;

/** How much is told (YEL_ASYNC_TRACE, read once). */
static int y_async_tracing(void) {
#if defined(Y_HOSTED)
	// (a guest's: none, so no printf)
	return 0;
#endif
	if (y_trace_level < 0) {
		const char *v = y_getenv("YEL_ASYNC_TRACE");
		y_trace_level = v ? atoi(v) : 0;
	}
	return y_trace_level;
}

static int64_t y_trace_ns(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + (int64_t)now.tv_nsec;
}

/** What is kept of a task (none: not traced). */
static y_task_trace *y_trace_of(y_async *t) {
	if (!t || t->id <= 0 || t->id > y_ntraces) return NULL;
	return &y_traces[t->id - 1];
}

static const char *y_task_name(y_async *t) { return t && t->name ? t->name : "a task"; }

/** What the task being stepped now waits for (where it parks: told when it does). */
static void y_trace_waits(const char *format, ...) {
	y_task_trace *tr = y_async_tracing() ? y_trace_of(y_async_now) : NULL;
	if (!tr) return;
	va_list args;
	va_start(args, format);
	vsnprintf(tr->waits, sizeof tr->waits, format, args);
	va_end(args);
}

/** A task started: numbered, and told (with the one that started it). */
static void y_trace_started(y_async *t) {
	t->id = ++y_task_count;
	if (!y_async_tracing()) return;
	if (y_ntraces == y_traces_cap) {
		y_traces_cap = y_traces_cap ? y_traces_cap * 2 : 64;
		y_traces = realloc(y_traces, (size_t)y_traces_cap * sizeof(y_task_trace));
		if (!y_traces) y_die("out of memory");
	}
	y_traces[y_ntraces++] = (y_task_trace){ y_trace_ns(), 0, 0, "", "" };
	if (y_async_now && y_async_now->id) {
		fprintf(stderr, "async: task %lld (%s) started by task %lld (%s)\n", (long long)t->id, y_task_name(t), (long long)y_async_now->id, y_task_name(y_async_now));
	} else {
		fprintf(stderr, "async: task %lld (%s) started\n", (long long)t->id, y_task_name(t));
	}
}

/** A task stepped: its time kept, and what came of it told (done, or parked and on what). */
static void y_trace_stepped(y_async *t, int64_t began) {
	y_task_trace *tr = y_trace_of(t);
	if (!tr) return;
	const int64_t now = y_trace_ns();
	tr->stepped += now - began;
	tr->steps++;
	if (t->done) {
		fprintf(stderr, "async: task %lld (%s) done: stepped %.3f ms in %lld steps, alive %.3f ms\n", (long long)t->id, y_task_name(t),
			(double)tr->stepped / 1e6, (long long)tr->steps, (double)(now - tr->started) / 1e6);
	} else if (y_trace_level >= 2 || tr->waits[0]) {
		fprintf(stderr, "async: task %lld (%s) parked: waits for %s\n", (long long)t->id, y_task_name(t), tr->waits[0] ? tr->waits : "a wake");
		if (tr->waits[0]) memcpy(tr->last, tr->waits, sizeof tr->last);
		tr->waits[0] = 0;
	}
}

/** Each task in the task list not done, and what it waits for, told after what. */
static void y_trace_waiting(const char *what) {
	if (!y_async_list) return;
	for (int64_t i = 0; i < y_async_list->len; i++) {
		y_async *t = ((y_async **)y_async_list->items)[i];
		y_task_trace *tr = y_trace_of(t);
		if (t->done || !tr) continue;
		fprintf(stderr, "async:   task %lld (%s) %s %s\n", (long long)t->id, y_task_name(t), what, tr->last[0] ? tr->last : "a wake");
	}
}

yunit yel_async_trace_deadlock(void) {
	y_out_flush();
	if (!y_async_tracing()) {
		y_out_text(2, "yel: YEL_ASYNC_TRACE=1 tells each task and what it waits for\n");
		return 0;
	}
	y_out_text(2, "async: a deadlock: every task waits, and nothing will wake one\n");
	y_trace_waiting("waits for");
	return 0;
}

yunit yel_async_trace_end(void) {
	if (y_async_tracing()) y_trace_waiting("never done: it waits for");
	return 0;
}

/** A task back in the ready queue (once; not once done). */
void y_wake(y_async *t) {
	if (!t || t->done || t->queued) return;
	if (y_trace_level >= 2 && y_trace_of(t)) fprintf(stderr, "async: task %lld (%s) woken\n", (long long)t->id, y_task_name(t));
	t->queued = 1;
	*(void **)y_list_push_slot(y_async_rooted(&y_async_ready)) = t;
}

/** The tasks waiting for t (it is done) woken. */
void y_async_finished(y_async *t) {
	ylist *w = y_async_rooted(&y_async_waits);
	int64_t kept = 0;
	for (int64_t i = 0; i + 1 < w->len; i += 2) {
		y_async *on = ((y_async **)w->items)[i];
		y_async *who = ((y_async **)w->items)[i + 1];
		if (on == t) y_wake(who);
		else if (!on->done && !who->done) {
			((y_async **)w->items)[kept++] = on;
			((y_async **)w->items)[kept++] = who;
		}
	}
	w->len = kept;
}

// ---- async context: the program's @(context) globals, each task's own. The globals hold the
// values of the task being stepped; the executor keeps each task's while another runs (its
// header's context: a copy of each, traced) and puts them back when it steps it again. A task
// started takes its starter's, as they are then

typedef struct { void *place; uint64_t size; uint64_t at; void (*scan)(void *); } y_context_var;
static y_context_var *y_context_vars;
static int64_t y_ncontext, y_context_cap;
static uint64_t y_context_bytes;

/** A context's values traced: each that holds a pointer. */
static void y_trace_context(void *obj) {
	for (int64_t i = 0; i < y_ncontext; i++) {
		if (y_context_vars[i].scan) y_context_vars[i].scan((char *)obj + y_context_vars[i].at);
	}
}

/** The started tasks' contexts, marked (the collector's: a task's header is the runtime's). */
static void y_trace_task_contexts(void) {
	if (!y_async_list) return;
	for (int64_t i = 0; i < y_async_list->len; i++) {
		y_async *t = ((y_async **)y_async_list->items)[i];
		if (t->context) y_mark(t->context);
	}
}

void y_context_global(void *place, size_t size, void (*scan)(void *)) {
	if (y_ncontext == 0) y_tracer(y_trace_task_contexts);
	if (y_ncontext == y_context_cap) {
		y_context_cap = y_context_cap ? y_context_cap * 2 : 4;
		y_context_vars = realloc(y_context_vars, (size_t)y_context_cap * sizeof(y_context_var));
		if (!y_context_vars) y_die("out of memory");
	}
	// (each at a 16-byte boundary: any value's alignment)
	y_context_vars[y_ncontext++] = (y_context_var){ place, (uint64_t)size, y_context_bytes, scan };
	y_context_bytes += (size + 15) & ~(uint64_t)15;
}

/** The globals' values now, copied into ctx. */
static void y_context_keep(void *ctx) {
	for (int64_t i = 0; i < y_ncontext; i++) memcpy((char *)ctx + y_context_vars[i].at, y_context_vars[i].place, y_context_vars[i].size);
}

/** The globals given ctx's values. */
static void y_context_enter(void *ctx) {
	for (int64_t i = 0; i < y_ncontext; i++) memcpy(y_context_vars[i].place, (char *)ctx + y_context_vars[i].at, y_context_vars[i].size);
}

/** A copy of the globals' values now (a context of its own). */
static void *y_context_copy(void) {
	void *ctx = y_new(y_context_bytes ? y_context_bytes : 1, y_trace_context);
	y_context_keep(ctx);
	return ctx;
}

/** A task started: its own, the executor's to step (ready at once); its owner the starter's (a
root's: itself); its async context a copy of its starter's. */
y_async *yel_async_spawn(y_async *t) {
	// rooted while the task list grows (the caller may hold it nowhere else), its context too
	// (traced through the task list only once the task is in it)
	Y_FRAME(2);
	ys_[0] = t;
	t->spawned = 1;
	if (y_ncontext > 0) {
		ys_[1] = y_context_copy();
		t->context = ys_[1];
	}
	t->owner = y_async_now && y_async_now->owner ? y_async_now->owner : t;
	y_trace_started(t);
	*(void **)y_list_push_slot(yel_async_tasks()) = t;
	y_wake(t);
	Y_POP();
	return t;
}

/** Whether a task of a root's is ready, and the first such taken out of the queue (a component's
call runs only its own: the others' are stepped by their calls). */
bool yel_async_has_ready_of(y_async *root) {
	if (!y_async_ready) return false;
	for (int64_t i = y_async_head; i < y_async_ready->len; i++) {
		if (((y_async **)y_async_ready->items)[i]->owner == root) return true;
	}
	return false;
}

y_async *yel_async_take_ready_of(y_async *root) {
	y_async **q = (y_async **)y_async_ready->items;
	for (int64_t i = y_async_head; i < y_async_ready->len; i++) {
		if (q[i]->owner == root) {
			y_async *t = q[i];
			memmove(&q[i], &q[i + 1], (size_t)(y_async_ready->len - i - 1) * sizeof(void *));
			y_async_ready->len--;
			t->queued = 0;
			return t;
		}
	}
	y_die("no ready task of this call");
}

/** The next ready task (the queue's first; its room let go once all are taken). */
y_async *yel_async_take_ready(void) {
	y_async *t = ((y_async **)y_async_ready->items)[y_async_head++];
	if (y_async_head == y_async_ready->len) {
		y_async_ready->len = 0;
		y_async_head = 0;
	}
	t->queued = 0;
	return t;
}

/** A task stepped by the executor (the one a wait in it parks); done: its waiters woken. Its async
context's values are the globals' while it runs (the ones before kept, and given back after). */
yunit yel_async_run(y_async *t) {
	if (t->done) return 0;
	Y_FRAME(1);
	ys_[0] = NULL;
	if (t->context) {
		ys_[0] = y_context_copy();
		y_context_enter(t->context);
	}
	y_async *was = y_async_now;
	y_async_now = t;
	const int64_t began = y_trace_level > 0 ? y_trace_ns() : 0;
	if (y_trace_level >= 2 && y_trace_of(t)) fprintf(stderr, "async: task %lld (%s) stepped\n", (long long)t->id, y_task_name(t));
	if (t->step(t)) {
		t->done = 1;
		t->order = ++y_async_order;
		y_async_finished(t);
	}
	if (y_trace_level > 0) y_trace_stepped(t, began);
	y_async_now = was;
	if (ys_[0]) {
		y_context_keep(t->context);
		y_context_enter(ys_[0]);
	}
	Y_POP();
	return 0;
}

/** What a wait does with what it waits for: a task of its own is stepped by the executor (the
waiter parks until it is done); anything else (an async call, a timer, a stream's read) is stepped
here. Whether it is done. */
bool yel_async_step(y_async *f) {
	if (f->done) return true;
	if (f->spawned) {
		ylist *w = y_async_rooted(&y_async_waits);
		// once for each pair (a wait a | b steps it each turn)
		for (int64_t i = 0; i + 1 < w->len; i += 2) {
			if (((y_async **)w->items)[i] == f && ((y_async **)w->items)[i + 1] == y_async_now) return false;
		}
		*(void **)y_list_push_slot(w) = f;
		*(void **)y_list_push_slot(w) = y_async_now;
		y_trace_waits("task %lld (%s)", (long long)f->id, y_task_name(f));
		return false;
	}
	if (f->step(f)) {
		f->done = 1;
		f->order = ++y_async_order;
	}
	return f->done != 0;
}

int32_t yel_async_first_of(int32_t n, y_async **tasks) {
	int32_t best = -1;
	int64_t at = 0;
	for (int32_t i = 0; i < n; i++) {
		y_async *f = tasks[i];
		if (yel_async_step(f) && (best < 0 || f->order < at)) {
			best = i;
			at = f->order;
		}
	}
	return best;
}

int32_t yel_async_first(int32_t n, ...) {
	y_async *tasks[n > 0 ? n : 1];
	va_list ap;
	va_start(ap, n);
	for (int32_t i = 0; i < n; i++) tasks[i] = va_arg(ap, y_async *);
	va_end(ap);
	return yel_async_first_of(n, tasks);
}

bool yel_async_has_ready(void) { return y_async_ready && y_async_head < y_async_ready->len; }

int32_t y_export_drive(y_async *root, bool (*run)(y_async *)) {
	for (;;) {
		bool done = run(root);
		// its result given once it is done (what it starts then is the call's: a pump), and the
		// call over once nothing of it is left
		if (done && !y_host_root(root)->returned) {
			y_async *was = y_async_now;
			y_async_now = root;
			y_host_root(root)->finish(root);
			y_async_now = was;
			y_host_root(root)->returned = 1;
			continue;
		}
		if (done && !y_async_owned_live(root) && !y_async_host_pending(root)) {
			y_host_root_end(root);
			yel_async_prune();
			y_turn_end();
			return 0;
		}
		break;
	}
	if (y_async_host_pending(root)) {
		const int32_t waits = (int32_t)(2u | (y_host_root(root)->set << 4));
		y_turn_end();
		return waits;
	}
	yel_async_trace_deadlock();
	y_die("every task of this call waits, and nothing will wake one: a deadlock");
}

void y_export_finish(y_async *root, void (*finish)(y_async *)) { y_host_root(root)->finish = finish; }

void y_async_release_by_step(y_async *f) {
	f->state = Y_RELEASE_STATE;
	(void)f->step(f);
}

yunit y_writer_close(ystream *w) { return yel_writer_close(0, 0, w); }

/** A generator stepped to its next value (in its frame's result); false once it is done. */
bool y_seq_next(y_async *g) {
	if (g->done) return false;
	if (g->step(g)) {
		g->done = 1;
		return false;
	}
	return true;
}

void y_future_release(y_async *f) {
	if (f->release && !f->done && f->state == 0) f->release(f);
}

/** The task being stepped (what a parked wait wakes). */
y_async *yel_async_current(void) { return y_async_now; }

yunit yel_async_wake(y_async *t) {
	y_wake(t);
	return 0;
}

/** A future done the second time it is stepped, its task woken at once (a turn for the others). */
int32_t y_async_yield_step(void *self) {
	y_async *a = self;
	if (a->state == 0) {
		a->state = 1;
		y_wake(y_async_now);
		y_trace_waits("its next turn (a yield)");
		return 0;
	}
	return 1;
}

y_async *yel_async_yield(void) {
	y_async *a = y_new(sizeof(y_async) + sizeof(yunit), NULL);
	a->step = y_async_yield_step;
	return a;
}

/** A future done the second time it is stepped, nothing woken: its task waits until something
wakes it (a stream, a timer: what parked it). */
int32_t y_async_park_step(void *self) {
	y_async *a = self;
	if (a->state == 0) {
		a->state = 1;
		return 0;
	}
	return 1;
}

y_async *yel_async_park(void) {
	y_async *a = y_new(sizeof(y_async) + sizeof(yunit), NULL);
	a->step = y_async_park_step;
	return a;
}

/** Milliseconds on a clock that only goes on. */
int64_t yel_async_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/** Milliseconds since 1970 by the wall clock, through the C library: natively the system's, in a
WASI 0.2 component its wall clock (what a guest a shell hosts has; std:time's now reads WASI 0.3's). */
int64_t yel_wall_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/** The task being stepped, woken at a time. */
// the timers' tasks, marked: a task a timer names stays until the timer is due (waking a task that
// is done by then does nothing)
void y_trace_timers(void) {
	for (int64_t i = 0; i < y_ntimers; i++) y_mark(y_timers[i].task);
}

yunit yel_async_timer(int64_t at) {
	if (!y_timers) y_tracer(y_trace_timers);
	if (y_ntimers == y_timers_cap) {
		y_timers_cap = y_timers_cap ? y_timers_cap * 2 : 16;
		y_timers = realloc(y_timers, (size_t)y_timers_cap * sizeof(y_timer));
		if (!y_timers) y_die("out of memory");
	}
	y_timers[y_ntimers++] = (y_timer){ at, y_async_now };
	y_trace_waits("a timer (%lld ms from now)", (long long)(at - yel_async_now_ms()));
	return 0;
}

/** The started tasks that are done, let go (a waiter still holding one keeps its frame). */
yunit yel_async_prune(void) {
	ylist *l = yel_async_tasks();
	int64_t kept = 0;
	for (int64_t i = 0; i < l->len; i++) {
		y_async *t = ((y_async **)l->items)[i];
		if (!t->done) ((y_async **)l->items)[kept++] = t;
	}
	l->len = kept;
	return 0;
}

void y_trace_stream(void *obj) {
	ystream *s = obj;
	y_mark(s->items);
	y_mark(s->reader);
	y_mark(s->writer);
}

void yel_stream_park_reader(ystream *s) {
	s->reader = y_async_now;
	y_trace_waits("a stream's items");
}

void yel_stream_park_writer(ystream *w) {
	w->writer = y_async_now;
	y_trace_waits("room in a stream (its reader to read)");
}

ystream *y_stream_new(int64_t size, y_scan scan, int64_t cap) {
	ystream *s = y_new(sizeof(ystream), y_trace_stream);
	Y_FRAME(1);
	ys_[0] = s;
	s->items = y_list_new(cap > 0 ? cap : 1, size, scan);
	Y_POP();
	s->cap = cap > 0 ? cap : 1;
	return s;
}

/** A list's first item, taken out (the rest moved up), into out (the caller's: a T). */
void *y_list_take_first(ylist *l, void *out) {
	if (l->len == 0) y_die("take-first of an empty list");
	memcpy(out, l->items, (size_t)l->size);
	memmove(l->items, l->items + l->size, (size_t)((l->len - 1) * l->size));
	l->len--;
	return out;
}

/** Whether a task of a root's (but the root) is not done: a pump still writing to the host. */
bool y_async_owned_live(y_async *root) {
	ylist *l = yel_async_tasks();
	for (int64_t i = 0; i < l->len; i++) {
		y_async *t = ((y_async **)l->items)[i];
		if (t != root && t->owner == root && !t->done) return true;
	}
	return false;
}

#if defined(__wasm__)
// ---- the host's waits (async-wasi.h)
y_hostwait *y_hostwaits;
int64_t y_nhostwaits, y_hostwaits_cap;
y_hostroot *y_hostroots;
int64_t y_nhostroots, y_hostroots_cap;

// the tasks and roots the host's waits name, marked: a task waiting for the host stays until the
// host is done with what it waits for, a root until its call is over (y_host_root_end)
void y_trace_host(void) {
	for (int64_t i = 0; i < y_nhostwaits; i++) {
		y_mark(y_hostwaits[i].task);
		y_mark(y_hostwaits[i].root);
	}
	for (int64_t i = 0; i < y_nhostroots; i++) y_mark(y_hostroots[i].root);
}

y_hostroot *y_host_root(y_async *root) {
	for (int64_t i = 0; i < y_nhostroots; i++) if (y_hostroots[i].root == root) return &y_hostroots[i];
	if (!y_hostroots) y_tracer(y_trace_host);
	if (y_nhostroots == y_hostroots_cap) {
		y_hostroots_cap = y_hostroots_cap ? y_hostroots_cap * 2 : 8;
		y_hostroots = realloc(y_hostroots, (size_t)y_hostroots_cap * sizeof(y_hostroot));
		if (!y_hostroots) y_die("out of memory");
	}
	y_hostroot *r = &y_hostroots[y_nhostroots++];
	r->root = root;
	r->set = y_cm_set_new();
	r->finish = NULL;
	r->returned = 0;
	return r;
}

/** A root's call over (EXIT given): its waitable set dropped, its entry and its waits' let go. */
void y_host_root_end(y_async *root) {
	for (int64_t i = 0; i < y_nhostroots; i++) {
		if (y_hostroots[i].root == root) {
			y_cm_set_drop(y_hostroots[i].set);
			y_hostroots[i] = y_hostroots[--y_nhostroots];
			break;
		}
	}
	int64_t kept = 0;
	for (int64_t i = 0; i < y_nhostwaits; i++) {
		if (y_hostwaits[i].root != root) y_hostwaits[kept++] = y_hostwaits[i];
	}
	y_nhostwaits = kept;
}

/** The task being stepped waits for a waitable the host completes (a subtask). */
void y_async_host_wait(uint32_t waitable) {
	y_async *root = y_async_now && y_async_now->owner ? y_async_now->owner : y_async_now;
	y_hostroot *r = y_host_root(root);
	y_cm_join(waitable, r->set);
	if (y_nhostwaits == y_hostwaits_cap) {
		y_hostwaits_cap = y_hostwaits_cap ? y_hostwaits_cap * 2 : 16;
		y_hostwaits = realloc(y_hostwaits, (size_t)y_hostwaits_cap * sizeof(y_hostwait));
		if (!y_hostwaits) y_die("out of memory");
	}
	y_hostwaits[y_nhostwaits++] = (y_hostwait){ waitable, y_async_now, root, 0, 0 };
	y_trace_waits("the host (waitable %u)", waitable);
}

/** The task being stepped waits, in place of the one that did, for a waitable already waited for
(a read another task began and left). */
void y_async_host_rewait(uint32_t waitable) {
	for (int64_t i = 0; i < y_nhostwaits; i++) {
		if (y_hostwaits[i].waitable == waitable) {
			y_hostwaits[i].task = y_async_now;
			return;
		}
	}
}

y_hostwait *y_host_find(uint32_t waitable) {
	for (int64_t i = 0; i < y_nhostwaits; i++) if (y_hostwaits[i].waitable == waitable) return &y_hostwaits[i];
	return NULL;
}

/** Whether the host is done with a waitable (it woke its task). */
bool y_async_host_done(uint32_t waitable) {
	y_hostwait *h = y_host_find(waitable);
	return h && h->done;
}

/** A subtask done with: let go (its entry, and the host's handle). */
void y_async_host_drop(uint32_t waitable) {
	for (int64_t i = 0; i < y_nhostwaits; i++) {
		if (y_hostwaits[i].waitable == waitable) {
			y_hostwaits[i] = y_hostwaits[--y_nhostwaits];
			break;
		}
	}
	y_cm_subtask_drop(waitable);
}

uint32_t y_async_host_code(uint32_t waitable) {
	y_hostwait *h = y_host_find(waitable);
	return h ? h->code : 0;
}

void y_async_host_forget(uint32_t waitable) {
	for (int64_t i = 0; i < y_nhostwaits; i++) {
		if (y_hostwaits[i].waitable == waitable) {
			y_hostwaits[i] = y_hostwaits[--y_nhostwaits];
			return;
		}
	}
}

/** Whether any task of a root's waits for the host. */
bool y_async_host_pending(y_async *root) {
	for (int64_t i = 0; i < y_nhostwaits; i++) if (y_hostwaits[i].root == root && !y_hostwaits[i].done) return true;
	return false;
}

/** An event from the host: a subtask returned (event 1, code 2) wakes the task waiting for it.
Its root (the call it is for). */
y_async *y_async_host_event(uint32_t event, uint32_t waitable, uint32_t code) {
	y_hostwait *h = y_host_find(waitable);
	if (!h) y_die("an event for a waitable this component does not wait for");
	// a subtask returned; a stream's or a future's read or write done
	if ((event == 1 && code == 2) || (event >= 2 && event <= 5)) {
		h->done = 1;
		h->code = code;
		y_wake(h->task);
	}
	return h->root;
}

/** The reactor: no task ready, so wait for the host (one event at a time, for a root whose task
waits for it) or sleep until the first timer, and wake what is due. False when nothing will wake
any task (every one waits for another, or for nothing). */
bool yel_async_await(void) {
	for (int64_t i = 0; i < y_nhostroots; i++) {
		y_async *root = y_hostroots[i].root;
		if (y_async_host_pending(root)) {
			uint32_t got[2];
			uint32_t event = y_cm_set_wait(y_hostroots[i].set, got);
			y_async_host_event(event, got[0], got[1]);
			return true;
		}
	}
	if (y_ntimers == 0) return false;
	int64_t first = y_timers[0].at;
	for (int64_t i = 1; i < y_ntimers; i++) if (y_timers[i].at < first) first = y_timers[i].at;
	int64_t now = yel_async_now_ms();
	if (first > now) {
		struct timespec ts = { (time_t)((first - now) / 1000), (long)((first - now) % 1000) * 1000000 };
		nanosleep(&ts, NULL);
		now = yel_async_now_ms();
	}
	int64_t kept = 0;
	for (int64_t i = 0; i < y_ntimers; i++) {
		if (y_timers[i].at <= now) y_wake(y_timers[i].task);
		else y_timers[kept++] = y_timers[i];
	}
	y_ntimers = kept;
	return true;
}

/** A stream the host writes, as a yel stream (its items lifted as they are read). */
ystream *y_stream_from_host(uint32_t end, uint32_t (*read)(uint32_t, uint8_t *, size_t),
	void (*lift)(ystream *, void *, int32_t), void (*drop)(uint32_t), size_t abi_size, int64_t size, y_scan scan) {
	ystream *s = y_stream_new(size, scan, 64);
	s->host = end;
	s->host_read = read;
	s->host_lift = lift;
	s->host_drop = drop;
	s->abi_size = abi_size;
	return s;
}

void y_trace_stream_filler(void *obj) { y_mark(((y_stream_filler *)obj)->s); }

/** The host's read into the stream done (code: count << 4 | state): its items lifted. */
int32_t y_stream_filled(ystream *s, uint32_t code) {
	s->host_busy = 0;
	s->host_lift(s, s->host_buf, (int32_t)(code >> 4));
	free(s->host_buf);
	s->host_buf = NULL;
	if ((code & 0xf) == 1 || s->host_release) {
		s->closed = 1;
		s->host_drop(s->host);
		s->host = 0;
	}
	return 1;
}

int32_t y_stream_fill_step(void *self) {
	y_stream_filler *f = self;
	ystream *s = f->s;
	if (f->h.state == 0) {
		if (s->host_busy) {
			// a read under way that an earlier one began and left: this one waits for it
			y_async_host_rewait(s->host);
			f->h.state = 1;
		} else {
			int64_t room = s->cap - s->items->len;
			if (room < 1) room = 1;
			s->host_buf = y_alloc(y_list_bytes(s->abi_size ? (int64_t)s->abi_size : 1, room));
			uint32_t st = s->host_read(s->host, s->host_buf, (size_t)room);
			if (st != Y_BLOCKED) return y_stream_filled(s, st);
			y_async_host_wait(s->host);
			s->host_busy = 1;
			f->h.state = 1;
			return 0;
		}
	}
	// done by another read meanwhile (its items are in the stream), or by the host now
	if (!s->host_busy) return 1;
	if (!y_async_host_done(s->host)) return 0;
	uint32_t code = y_async_host_code(s->host);
	y_async_host_forget(s->host);
	return y_stream_filled(s, code);
}

/** A future done once more of the host's items are read into the stream (or it is closed). */
y_async *yel_stream_fill(ystream *s) {
	y_stream_filler *f = y_new(sizeof(y_stream_filler), y_trace_stream_filler);
	Y_FRAME(1);
	ys_[0] = f;
	f->s = s;
	f->h.step = y_stream_fill_step;
	Y_POP();
	return &f->h;
}

/** A stream no one reads any more (the compiler's, where its local is last used): the host's end
let go (its writer told), or once the read under way is done. */
void y_stream_release(ystream *s) {
	if (!s->host) return;
	if (s->host_busy) {
		s->host_release = 1;
		return;
	}
	s->host_drop(s->host);
	s->host = 0;
	s->closed = 1;
}

void y_trace_stream_pump(void *obj) {
	y_stream_pump *p = obj;
	y_mark(p->s);
	y_mark(p->held);
	y_mark(p->fill);
}

int32_t y_stream_pump_step(void *self) {
	y_stream_pump *p = self;
	for (;;) {
		if (p->buf) {
			uint32_t code;
			if (p->h.state == 1) {
				if (!y_async_host_done(p->end)) return 0;
				code = y_async_host_code(p->end);
				y_async_host_forget(p->end);
				p->h.state = 0;
			} else {
				code = p->write(p->end, p->buf + (size_t)p->off * p->abi_size, (size_t)(p->n - p->off));
				if (code == Y_BLOCKED) {
					y_async_host_wait(p->end);
					p->h.state = 1;
					return 0;
				}
			}
			p->off += (int32_t)(code >> 4);
			// the reader gone: nothing more to write
			if ((code & 0xf) == 1 || p->off >= p->n) {
				free(p->buf);
				p->buf = NULL;
				p->held = NULL;
				if ((code & 0xf) == 1) {
					p->drop(p->end);
					return 1;
				}
			}
			continue;
		}
		ystream *s = p->s;
		if (s->items->len > 0) {
			// the items given the host, kept (held) until it has copied them
			ylist *items = s->items;
			p->held = items;
			s->items = y_list_new(s->cap, items->size, items->scan);
			p->n = (int32_t)items->len;
			p->off = 0;
			p->buf = y_alloc(y_list_bytes(p->abi_size ? (int64_t)p->abi_size : 1, p->n));
			p->lower(items->items, p->buf, p->n);
			y_wake(s->writer);
			s->writer = NULL;
			continue;
		}
		if (s->closed) {
			p->drop(p->end);
			return 1;
		}
		// a stream the host writes, part read here: the rest read from the host as it comes
		if (s->host) {
			if (!p->fill) p->fill = yel_stream_fill(s);
			if (!yel_async_step(p->fill)) return 0;
			p->fill = NULL;
			continue;
		}
		s->reader = y_async_now;
		y_trace_waits("a stream's items");
		return 0;
	}
}

/** A yel stream given the host: a stream the host writes, none of it read here, is given back as
it is (its end moves, nothing is copied); any other, a new pair (its readable end given the host)
and a task (the call's) writing the stream's items to the writable end. */
uint32_t y_stream_to_host(ystream *s, uint64_t (*new_pair)(void), uint32_t (*write)(uint32_t, const uint8_t *, size_t),
	void (*lower)(void *, void *, int32_t), void (*drop)(uint32_t), size_t abi_size) {
	if (s->host && s->items->len == 0 && !s->closed && !s->host_busy) {
		uint32_t end = s->host;
		s->host = 0;
		s->closed = 1;
		return end;
	}
	uint64_t pair = new_pair();
	Y_FRAME(1);
	ys_[0] = s;
	y_stream_pump *p = y_new(sizeof(y_stream_pump), y_trace_stream_pump);
	Y_POP();
	p->h.step = y_stream_pump_step;
	p->s = s;
	p->end = (uint32_t)(pair >> 32);
	p->write = write;
	p->lower = lower;
	p->drop = drop;
	p->abi_size = abi_size;
	yel_async_spawn(&p->h);
	return (uint32_t)pair;
}

#else
// ---- the reactor (async-native.h)

// requests in flight on the loop (each wakes its task when it finishes)
int64_t y_uv_pending;

// the timer the loop waits on until the first of the executor's timers is due
uv_timer_t y_uv_timer;
bool y_uv_timer_ready;

void y_uv_timer_fired(uv_timer_t *timer) { (void)timer; }

/** The reactor: no task ready, so the loop runs until a timer is due or a request finishes, and the
tasks those wake are woken. False when nothing will wake any task (every one waits for another, or
for nothing). */
bool yel_async_await(void) {
	if (y_ntimers == 0 && y_uv_pending == 0) return false;
	uv_loop_t *loop = uv_default_loop();
	if (y_ntimers > 0) {
		int64_t first = y_timers[0].at;
		for (int64_t i = 1; i < y_ntimers; i++) if (y_timers[i].at < first) first = y_timers[i].at;
		const int64_t now = yel_async_now_ms();
		if (first > now) {
			if (!y_uv_timer_ready) {
				uv_timer_init(loop, &y_uv_timer);
				y_uv_timer_ready = true;
			}
			uv_timer_start(&y_uv_timer, y_uv_timer_fired, (uint64_t)(first - now), 0);
			uv_run(loop, UV_RUN_ONCE);
			uv_timer_stop(&y_uv_timer);
		} else {
			uv_run(loop, UV_RUN_NOWAIT);
		}
	} else {
		uv_run(loop, UV_RUN_ONCE);
	}
	const int64_t now = yel_async_now_ms();
	int64_t kept = 0;
	for (int64_t i = 0; i < y_ntimers; i++) {
		if (y_timers[i].at <= now) y_wake(y_timers[i].task);
		else y_timers[kept++] = y_timers[i];
	}
	y_ntimers = kept;
	return true;
}

// ---- descriptors watched: a task waiting for one to be readable or writable (yel_async_ready).
// libuv allows one poll to a descriptor, so each has one watch: who waits to read, who to write,
// the poll narrowed to what is waited for, and stopped (its handle closed) once nothing is

// the descriptors listening (yel_host_socket_listen's)
static int *y_listening;
static int64_t y_listening_count;

typedef struct y_watch {
	uv_poll_t poll;
	int fd;
	y_async *reader;
	y_async *writer;
	struct y_watch *next;
} y_watch;

static y_watch *y_watches;

static void y_trace_watches(void) {
	for (y_watch *w = y_watches; w; w = w->next) {
		if (w->reader) y_mark(w->reader);
		if (w->writer) y_mark(w->writer);
	}
}

static void y_watch_closed(uv_handle_t *handle) { free(handle->data); }

// the watch let go: out of the list, its poll stopped and closed (its memory freed once libuv is done)
static void y_watch_end(y_watch *done) {
	for (y_watch **at = &y_watches; *at; at = &(*at)->next) {
		if (*at == done) {
			*at = done->next;
			break;
		}
	}
	uv_poll_stop(&done->poll);
	uv_close((uv_handle_t *)&done->poll, y_watch_closed);
}

// the poll asks for what its waiters wait for; none, and the watch ends
static void y_watch_update(y_watch *w);

static void y_watch_fired(uv_poll_t *poll, int status, int events) {
	y_watch *w = poll->data;
	// an error or a hang-up wakes both: their read or write then says what happened
	const bool all = status < 0 || (events & UV_DISCONNECT);
	if (w->reader && (all || (events & UV_READABLE))) {
		y_uv_pending--;
		y_wake(w->reader);
		w->reader = NULL;
	}
	if (w->writer && (all || (events & UV_WRITABLE))) {
		y_uv_pending--;
		y_wake(w->writer);
		w->writer = NULL;
	}
	y_watch_update(w);
}

static void y_watch_update(y_watch *w) {
	const int events = (w->reader ? UV_READABLE : 0) | (w->writer ? UV_WRITABLE : 0);
	if (events == 0) {
		y_watch_end(w);
		return;
	}
	uv_poll_start(&w->poll, events | UV_DISCONNECT, y_watch_fired);
}

/** The task being stepped, to be woken once fd can be read (events 1) or written (2) without
blocking, or is at its end, or failed (the call then tells); true when it waits so (it parks next).
False for a descriptor libuv cannot poll (a regular file, which never waits): it goes on at once. */
bool yel_async_ready(int64_t fd, int64_t events) {
	static bool traced;
	if (!traced) {
		y_tracer(y_trace_watches);
		traced = true;
	}
	y_watch *w = y_watches;
	while (w && w->fd != (int)fd) w = w->next;
	if (!w) {
		w = malloc(sizeof(y_watch));
		if (!w) y_die("out of memory");
		if (uv_poll_init(uv_default_loop(), &w->poll, (int)fd) != 0) {
			free(w);
			return false;
		}
		w->poll.data = w;
		w->fd = (int)fd;
		w->reader = NULL;
		w->writer = NULL;
		w->next = y_watches;
		y_watches = w;
	}
	if (events & 1) {
		if (!w->reader) y_uv_pending++;
		w->reader = y_async_now;
		y_trace_waits("descriptor %d to be read", (int)fd);
	}
	if (events & 2) {
		if (!w->writer) y_uv_pending++;
		w->writer = y_async_now;
		y_trace_waits("descriptor %d to be written", (int)fd);
	}
	y_watch_update(w);
	return true;
}

/** fd about to be closed: its watch (if any) ended first, its waiters woken (their call then fails),
and it listens no more. */
yunit yel_async_forget(int64_t fd) {
	for (int64_t index = 0; index < y_listening_count; index++) {
		if (y_listening[index] == (int)fd) {
			y_listening[index] = y_listening[--y_listening_count];
			break;
		}
	}
	for (y_watch *w = y_watches; w; w = w->next) {
		if (w->fd != (int)fd) continue;
		if (w->reader) {
			y_uv_pending--;
			y_wake(w->reader);
		}
		if (w->writer) {
			y_uv_pending--;
			y_wake(w->writer);
		}
		y_watch_end(w);
		break;
	}
	return 0;
}

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

// ---- the reactor's requests: a blocking call of the system run on libuv's thread pool, the task
// that asked woken when it is done (yel_pool_*). The call's own: its result and errno, numbers only
// (a worker touches nothing of yel's heap: a path is C's, a buffer one the waiting task's frame holds)

enum { Y_POOL_PREAD, Y_POOL_PWRITE, Y_POOL_OPENAT, Y_POOL_MKDIRAT, Y_POOL_UNLINKAT, Y_POOL_RENAMEAT, Y_POOL_STAT, Y_POOL_LOOKUP };

typedef struct y_request {
	uv_work_t work;
	int op;
	int64_t args[4];
	y_async *task;
	int64_t result;
	int64_t error;
	bool done;
	struct y_request *next;
} y_request;

// every request not freed yet, so the collector keeps the tasks they wake
static y_request *y_requests;

static void y_trace_requests(void) {
	for (y_request *r = y_requests; r; r = r->next) y_mark(r->task);
}

// fstatat or fstat (path NULL), written as the native helpers number a stat (yn_fs_stat's)
static int y_stat_into(int fd, const char *path, bool follow, int64_t *o) {
	struct stat st;
	const int r = path ? fstatat(fd, path, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW) : fstat(fd, &st);
	if (r != 0) return -1;
	o[0] = yn_fs_kind(st.st_mode);
	o[1] = (int64_t)st.st_nlink;
	o[2] = (int64_t)st.st_size;
#if defined(__APPLE__)
	o[3] = st.st_atimespec.tv_sec, o[4] = st.st_atimespec.tv_nsec;
	o[5] = st.st_mtimespec.tv_sec, o[6] = st.st_mtimespec.tv_nsec;
	o[7] = st.st_ctimespec.tv_sec, o[8] = st.st_ctimespec.tv_nsec;
#else
	o[3] = st.st_atim.tv_sec, o[4] = st.st_atim.tv_nsec;
	o[5] = st.st_mtim.tv_sec, o[6] = st.st_mtim.tv_nsec;
	o[7] = st.st_ctim.tv_sec, o[8] = st.st_ctim.tv_nsec;
#endif
	return 0;
}

// a name's addresses (getaddrinfo: TCP's, any family), into out: each 9 numbers, its family (4 or 6)
// then its 4 bytes or 8 segments, in the order given, each once and none an IPv4-mapped IPv6
// address (wasi:sockets' ip-name-lookup says so), at most capacity. Their count, or what failed:
// -1 no such name, -2 a temporary failure, -3 a permanent one, -4 another
static int64_t y_lookup_into(const char *name, int64_t *out, int64_t capacity) {
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *found = NULL;
	const int code = getaddrinfo(name, NULL, &hints, &found);
	if (code != 0) {
		if (code == EAI_NONAME) return -1;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
		if (code == EAI_NODATA) return -1;
#endif
#if defined(EAI_ADDRFAMILY)
		if (code == EAI_ADDRFAMILY) return -1;
#endif
		if (code == EAI_AGAIN) return -2;
		if (code == EAI_FAIL) return -3;
		return -4;
	}
	int64_t count = 0;
	for (struct addrinfo *a = found; a && count < capacity; a = a->ai_next) {
		int64_t one[9] = { 0 };
		if (a->ai_family == AF_INET) {
			const uint8_t *b = (const uint8_t *)&((struct sockaddr_in *)a->ai_addr)->sin_addr;
			one[0] = 4;
			for (int i = 0; i < 4; i++) one[1 + i] = b[i];
		} else if (a->ai_family == AF_INET6) {
			const uint8_t *b = ((struct sockaddr_in6 *)a->ai_addr)->sin6_addr.s6_addr;
			// (::ffff:a.b.c.d: an IPv4 address, which wasi:sockets never gives this way)
			bool mapped = true;
			for (int i = 0; i < 10; i++) mapped = mapped && b[i] == 0;
			if (mapped && b[10] == 0xff && b[11] == 0xff) continue;
			one[0] = 6;
			for (int i = 0; i < 8; i++) one[1 + i] = (int64_t)b[2 * i] << 8 | b[2 * i + 1];
		} else {
			continue;
		}
		bool seen = false;
		for (int64_t k = 0; k < count && !seen; k++) seen = memcmp(out + k * 9, one, sizeof one) == 0;
		if (!seen) memcpy(out + (count++) * 9, one, sizeof one);
	}
	freeaddrinfo(found);
	return count > 0 ? count : -1;
}

// on a worker: the call, and its errno where it fails
static void y_request_run(uv_work_t *work) {
	y_request *r = work->data;
	const int64_t *a = r->args;
	int64_t result = -1;
	errno = 0;
	switch (r->op) {
	case Y_POOL_PREAD: result = pread((int)a[0], (void *)(uintptr_t)a[1], (size_t)a[2], (off_t)a[3]); break;
	case Y_POOL_PWRITE: result = pwrite((int)a[0], (const void *)(uintptr_t)a[1], (size_t)a[2], (off_t)a[3]); break;
	case Y_POOL_OPENAT: result = openat((int)a[0], (const char *)(uintptr_t)a[1], (int)a[2], 0666); break;
	case Y_POOL_MKDIRAT: result = mkdirat((int)a[0], (const char *)(uintptr_t)a[1], 0777); break;
	case Y_POOL_UNLINKAT: result = unlinkat((int)a[0], (const char *)(uintptr_t)a[1], (int)a[2]); break;
	case Y_POOL_RENAMEAT:
		result = renameat((int)a[0], (const char *)(uintptr_t)a[1], (int)a[2], (const char *)(uintptr_t)a[3]);
		break;
	case Y_POOL_STAT:
		result = y_stat_into((int)a[0], a[1] ? (const char *)(uintptr_t)a[1] : NULL, a[2] != 0, (int64_t *)(uintptr_t)a[3]);
		break;
	case Y_POOL_LOOKUP:
		result = y_lookup_into((const char *)(uintptr_t)a[0], (int64_t *)(uintptr_t)a[1], a[2]);
		break;
	}
	r->result = result;
	r->error = result < 0 ? errno : 0;
}

// on the loop's thread: done, its task woken
static void y_request_finished(uv_work_t *work, int status) {
	(void)status;
	y_request *r = work->data;
	r->done = true;
	y_uv_pending--;
	y_wake(r->task);
}

static int64_t y_pool(int op, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
	static bool traced;
	if (!traced) {
		y_tracer(y_trace_requests);
		traced = true;
	}
	y_request *r = calloc(1, sizeof(y_request));
	if (!r) y_die("out of memory");
	r->work.data = r;
	r->op = op;
	r->args[0] = a0, r->args[1] = a1, r->args[2] = a2, r->args[3] = a3;
	r->task = y_async_now;
	y_trace_waits(op == Y_POOL_LOOKUP ? "a name's addresses (getaddrinfo, on the thread pool)" : "a file operation (on the thread pool)");
	r->next = y_requests;
	y_requests = r;
	y_uv_pending++;
	if (uv_queue_work(uv_default_loop(), &r->work, y_request_run, y_request_finished) != 0) {
		// (not queued: done at once, as an error)
		y_uv_pending--;
		r->done = true;
		r->result = -1;
		r->error = EAGAIN;
	}
	return (int64_t)(uintptr_t)r;
}

/** pread, pwrite, openat, mkdirat, unlinkat, renameat and fstatat begun on the thread pool: the
request, which its task parks for until yel_request_done (its result and errno then read, and the
request freed with yel_request_free). Paths are C strings, freed by the caller after it is done. */
int64_t yel_pool_pread(int64_t fd, int64_t at, int64_t count, int64_t offset) { return y_pool(Y_POOL_PREAD, fd, at, count, offset); }
int64_t yel_pool_pwrite(int64_t fd, int64_t at, int64_t count, int64_t offset) { return y_pool(Y_POOL_PWRITE, fd, at, count, offset); }
int64_t yel_pool_openat(int64_t dir, int64_t path, int64_t options) { return y_pool(Y_POOL_OPENAT, dir, path, options, 0); }
int64_t yel_pool_mkdirat(int64_t dir, int64_t path) { return y_pool(Y_POOL_MKDIRAT, dir, path, 0, 0); }
int64_t yel_pool_unlinkat(int64_t dir, int64_t path, int64_t flags) { return y_pool(Y_POOL_UNLINKAT, dir, path, flags, 0); }
int64_t yel_pool_renameat(int64_t from_dir, int64_t from, int64_t to_dir, int64_t to) { return y_pool(Y_POOL_RENAMEAT, from_dir, from, to_dir, to); }
int64_t yel_pool_stat(int64_t fd, int64_t path, int64_t follow, int64_t out) { return y_pool(Y_POOL_STAT, fd, path, follow, out); }
/** A name's addresses looked up on the thread pool (y_lookup_into: into out, at most capacity; the name a C string). */
int64_t yel_pool_lookup(int64_t name, int64_t out, int64_t capacity) { return y_pool(Y_POOL_LOOKUP, name, out, capacity, 0); }

bool yel_request_done(int64_t request) { return ((y_request *)(uintptr_t)request)->done; }
int64_t yel_request_result(int64_t request) { return ((y_request *)(uintptr_t)request)->result; }
int64_t yel_request_error(int64_t request) { return ((y_request *)(uintptr_t)request)->error; }

yunit yel_request_free(int64_t request) {
	y_request *done = (y_request *)(uintptr_t)request;
	for (y_request **at = &y_requests; *at; at = &(*at)->next) {
		if (*at == done) {
			*at = done->next;
			break;
		}
	}
	free(done);
	return 0;
}

// ---- sockets that never block: their calls give EAGAIN (EINPROGRESS for a connect) where they
// would, and the task waits on the descriptor's watch

/** fd made non-blocking (and, where the system has it, never a SIGPIPE on a write to a closed peer). */
int64_t yel_host_nonblocking(int64_t fd) {
	const int flags = fcntl((int)fd, F_GETFL, 0);
	if (flags < 0 || fcntl((int)fd, F_SETFL, flags | O_NONBLOCK) < 0) return -errno;
#if defined(SO_NOSIGPIPE)
	int one = 1;
	setsockopt((int)fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	return 0;
}

/** Up to count bytes at at sent on socket fd: how many, or -errno (-EAGAIN: none fit now). */
int64_t yel_host_socket_send(int64_t fd, int64_t at, int64_t count) {
#if defined(MSG_NOSIGNAL)
	const ssize_t sent = send((int)fd, (const void *)(uintptr_t)at, (size_t)count, MSG_NOSIGNAL);
#else
	const ssize_t sent = send((int)fd, (const void *)(uintptr_t)at, (size_t)count, 0);
#endif
	return sent < 0 ? -errno : (int64_t)sent;
}

/** Up to count bytes of socket fd into at: how many, 0 at its end, or -errno (-EAGAIN: none yet). */
int64_t yel_host_socket_receive(int64_t fd, int64_t at, int64_t count) {
	const ssize_t got = recv((int)fd, (void *)(uintptr_t)at, (size_t)count, 0);
	return got < 0 ? -errno : (int64_t)got;
}

// (the descriptors listening, y_listening: what get-is-listening answers by where the system
// cannot say, as macOS has no SO_ACCEPTCONN; one leaves it when it is closed: yel_async_forget)

/** fd listening, backlog connections queued at most: 0, or -errno. */
int64_t yel_host_socket_listen(int64_t fd, int64_t backlog) {
	if (listen((int)fd, (int)backlog) < 0) return -errno;
	int *grown = realloc(y_listening, (size_t)(y_listening_count + 1) * sizeof(int));
	if (!grown) y_die("out of memory");
	y_listening = grown;
	y_listening[y_listening_count++] = (int)fd;
	return 0;
}

/** Whether fd listens: the system's answer (SO_ACCEPTCONN), else whether it was made to. */
bool yel_host_socket_is_listening(int64_t fd) {
	int value = 0;
	socklen_t length = sizeof value;
	if (getsockopt((int)fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &length) == 0) return value != 0;
	for (int64_t index = 0; index < y_listening_count; index++) {
		if (y_listening[index] == (int)fd) return true;
	}
	return false;
}

/** A connection fd has queued, non-blocking: its descriptor, or -errno (-EAGAIN: none yet). */
int64_t yel_host_socket_accept(int64_t fd) {
	const int accepted = accept((int)fd, NULL, NULL);
	if (accepted < 0) return -errno;
	const int64_t made = yel_host_nonblocking(accepted);
	if (made < 0) {
		close(accepted);
		return made;
	}
	return accepted;
}

/** fd's writing ended (the peer reads its end): 0, or -errno. */
int64_t yel_host_socket_shutdown_write(int64_t fd) { return shutdown((int)fd, SHUT_WR) < 0 ? -errno : 0; }

/** What a non-blocking connect ended with: 0, or the errno (SO_ERROR). */
int64_t yel_host_socket_error(int64_t fd) {
	int error = 0;
	socklen_t length = sizeof error;
	if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) return errno;
	return error;
}

int64_t yel_host_e_again(void) { return EAGAIN; }
int64_t yel_host_e_in_progress(void) { return EINPROGRESS; }

/** Up to count bytes of fd at at (read(2)): how many, 0 at its end, -1 for an error. */
int64_t yel_host_read(int64_t fd, int64_t at, int64_t count) { return (int64_t)read((int)fd, (void *)(uintptr_t)at, (size_t)count); }

/** count bytes at at written to stdout or stderr (2 for stderr), after what print wrote, and flushed. */
int64_t yel_host_write_out(int64_t fd, int64_t at, int64_t count) {
#if defined(Y_HOSTED) && defined(__wasip2__)
	return y_out((int)fd, (const void *)(uintptr_t)at, (size_t)count) ? count : -1;
#else
	FILE *out = fd == 2 ? stderr : stdout;
	const size_t wrote = fwrite((const void *)(uintptr_t)at, 1, (size_t)count, out);
	if (fflush(out) != 0 || wrote != (size_t)count) return -1;
	return (int64_t)wrote;
#endif
}

Y_NORETURN void y_no_host(void) { y_die("this waits for a host: build it as a component (WASI 0.3)"); }

y_hostroot *y_host_root(y_async *root) {
	(void)root;
	y_no_host();
}

void y_host_root_end(y_async *root) {
	(void)root;
	y_no_host();
}

void y_async_host_wait(uint32_t waitable) {
	(void)waitable;
	y_no_host();
}

bool y_async_host_done(uint32_t waitable) {
	(void)waitable;
	y_no_host();
}

uint32_t y_async_host_code(uint32_t waitable) {
	(void)waitable;
	y_no_host();
}

void y_async_host_forget(uint32_t waitable) { (void)waitable; }

void y_async_host_drop(uint32_t waitable) { (void)waitable; }

bool y_async_host_pending(y_async *root) {
	(void)root;
	return false;
}

y_async *y_async_host_event(uint32_t event, uint32_t waitable, uint32_t code) {
	(void)event;
	(void)waitable;
	(void)code;
	y_no_host();
}

ystream *y_stream_from_host(uint32_t end, uint32_t (*read)(uint32_t, uint8_t *, size_t),
	void (*lift)(ystream *, void *, int32_t), void (*drop)(uint32_t), size_t abi_size, int64_t size, y_scan scan) {
	(void)end, (void)read, (void)lift, (void)drop, (void)abi_size, (void)size, (void)scan;
	y_no_host();
}

uint32_t y_stream_to_host(ystream *s, uint64_t (*new_pair)(void), uint32_t (*write)(uint32_t, const uint8_t *, size_t),
	void (*lower)(void *, void *, int32_t), void (*drop)(uint32_t), size_t abi_size) {
	(void)s, (void)new_pair, (void)write, (void)lower, (void)drop, (void)abi_size;
	y_no_host();
}

y_async *yel_stream_fill(ystream *s) {
	(void)s;
	y_no_host();
}

// ---- WASI natively (wasi-native.h)

// a kind of file as the host numbers it: 0 regular, 1 directory, 2 symbolic link, 3 block device,
// 4 character device, 5 fifo, 6 socket, 7 other
int64_t yn_fs_kind(mode_t m) {
  return S_ISREG(m) ? 0 : S_ISDIR(m) ? 1 : S_ISLNK(m) ? 2 : S_ISBLK(m) ? 3 : S_ISCHR(m) ? 4 : S_ISFIFO(m) ? 5 : S_ISSOCK(m) ? 6 : 7;
}

// fd's stat (path ""), or that of path in fd's directory, into out: kind, link count, size, then
// the access, modification and status change times (seconds, nanoseconds each); 0, or -errno
int64_t yn_fs_stat(int64_t fd, ystr path, bool follow, int64_t out) {
  struct stat st;
  int r;
  if (path.len == 0) {
    r = fstat((int)fd, &st);
  } else {
    char *p = y_cstr(path);
    r = fstatat((int)fd, p, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW);
    free(p);
  }
  if (r != 0) return -errno;
  int64_t *o = (int64_t *)(uintptr_t)out;
  o[0] = yn_fs_kind(st.st_mode);
  o[1] = (int64_t)st.st_nlink;
  o[2] = (int64_t)st.st_size;
#if defined(__APPLE__)
  o[3] = st.st_atimespec.tv_sec, o[4] = st.st_atimespec.tv_nsec;
  o[5] = st.st_mtimespec.tv_sec, o[6] = st.st_mtimespec.tv_nsec;
  o[7] = st.st_ctimespec.tv_sec, o[8] = st.st_ctimespec.tv_nsec;
#else
  o[3] = st.st_atim.tv_sec, o[4] = st.st_atim.tv_nsec;
  o[5] = st.st_mtim.tv_sec, o[6] = st.st_mtim.tv_nsec;
  o[7] = st.st_ctim.tv_sec, o[8] = st.st_ctim.tv_nsec;
#endif
  return 0;
}

// fd's directory opened for its entries (its DIR as a number: over a dup, so closing it leaves fd);
// -errno where it cannot be
int64_t yn_fs_dir_open(int64_t fd) {
  const int copy = dup((int)fd);
  if (copy < 0) return -errno;
  DIR *d = fdopendir(copy);
  if (!d) {
    const int e = errno;
    close(copy);
    return -e;
  }
  rewinddir(d);
  return (int64_t)(uintptr_t)d;
}

// the next entry's name ("" at the end; . and .. passed over) and its kind (as yn_fs_kind's, into
// *kind)
ystr yn_fs_dir_next(int64_t dir, int64_t kind) {
  for (struct dirent *e; (e = readdir((DIR *)(uintptr_t)dir));) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    int64_t k = 7;
    switch (e->d_type) {
    case DT_REG: k = 0; break;
    case DT_DIR: k = 1; break;
    case DT_LNK: k = 2; break;
    case DT_BLK: k = 3; break;
    case DT_CHR: k = 4; break;
    case DT_FIFO: k = 5; break;
    case DT_SOCK: k = 6; break;
    }
    *(int64_t *)(uintptr_t)kind = k;
    return y_str_of(e->d_name, (int64_t)strlen(e->d_name));
  }
  return (ystr){0, ""};
}

// ---- wasi:sockets natively: what std's host (runtime/std/core/host/host-sockets.yel) cannot say in yel, a
// socket address as numbers (12 of them): its family (0 ipv4, 1 ipv6), port, flow info and scope
// id, then the address's 4 bytes or 8 segments

// the address the numbers at say, as the system's (its length)
static socklen_t y_socket_address(int64_t at, struct sockaddr_storage *storage) {
  const int64_t *parts = (const int64_t *)(uintptr_t)at;
  socklen_t length;
  if (parts[0] == 0) {
    struct sockaddr_in *address = (struct sockaddr_in *)storage;
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)parts[1]);
    uint8_t *bytes = (uint8_t *)&address->sin_addr;
    for (int index = 0; index < 4; index++)
      bytes[index] = (uint8_t)parts[4 + index];
    length = sizeof *address;
  } else {
    struct sockaddr_in6 *address = (struct sockaddr_in6 *)storage;
    address->sin6_family = AF_INET6;
    address->sin6_port = htons((uint16_t)parts[1]);
    address->sin6_flowinfo = htonl((uint32_t)parts[2]);
    address->sin6_scope_id = (uint32_t)parts[3];
    for (int index = 0; index < 8; index++) {
      address->sin6_addr.s6_addr[2 * index] = (uint8_t)(parts[4 + index] >> 8);
      address->sin6_addr.s6_addr[2 * index + 1] = (uint8_t)parts[4 + index];
    }
    length = sizeof *address;
  }
  return length;
}

// fd bound to the address the numbers at say (the address reusable at once); 0, or -errno
int64_t yn_socket_bind(int64_t fd, int64_t at) {
  struct sockaddr_storage storage = {0};
  const socklen_t length = y_socket_address(at, &storage);
  int one = 1;
  setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  return bind((int)fd, (struct sockaddr *)&storage, length) < 0 ? -errno : 0;
}

// fd's connection to the address the numbers at say begun (a non-blocking socket's: -EINPROGRESS
// while it goes on); 0, or -errno
int64_t yn_socket_connect(int64_t fd, int64_t at) {
  struct sockaddr_storage storage = {0};
  const socklen_t length = y_socket_address(at, &storage);
  return connect((int)fd, (struct sockaddr *)&storage, length) < 0 ? -errno : 0;
}

// an address as numbers, written at
static void y_socket_parts(const struct sockaddr_storage *storage_at, int64_t at);

// fd's local address as numbers, written at (before it is bound: its family, port 0); 0, or -errno
int64_t yn_socket_name(int64_t fd, int64_t at) {
  struct sockaddr_storage storage = {0};
  socklen_t length = sizeof storage;
  if (getsockname((int)fd, (struct sockaddr *)&storage, &length) < 0)
    return -errno;
  y_socket_parts(&storage, at);
  return 0;
}

// the address fd is connected to, as numbers written at; 0, or -errno (-ENOTCONN: none)
int64_t yn_socket_peer(int64_t fd, int64_t at) {
  struct sockaddr_storage storage = {0};
  socklen_t length = sizeof storage;
  if (getpeername((int)fd, (struct sockaddr *)&storage, &length) < 0)
    return -errno;
  y_socket_parts(&storage, at);
  return 0;
}

static void y_socket_parts(const struct sockaddr_storage *storage_at, int64_t at) {
  int64_t *parts = (int64_t *)(uintptr_t)at;
  const struct sockaddr_storage storage = *storage_at;
  if (storage.ss_family == AF_INET) {
    const struct sockaddr_in *address = (const struct sockaddr_in *)&storage;
    const uint8_t *bytes = (const uint8_t *)&address->sin_addr;
    parts[0] = 0;
    parts[1] = ntohs(address->sin_port);
    for (int index = 0; index < 4; index++)
      parts[4 + index] = bytes[index];
  } else {
    const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)&storage;
    parts[0] = 1;
    parts[1] = ntohs(address->sin6_port);
    parts[2] = ntohl(address->sin6_flowinfo);
    parts[3] = address->sin6_scope_id;
    for (int index = 0; index < 8; index++)
      parts[4 + index] = address->sin6_addr.s6_addr[2 * index] << 8 | address->sin6_addr.s6_addr[2 * index + 1];
  }
}

// ---- std's native hosts' calls of the system (wasi-native.h declares them)
int64_t yel_host_cli_argument_count(void) { return ((int64_t)y_argc); }
int64_t yel_host_cli_argument(int64_t a1) { return ((int64_t)(uintptr_t)y_argv[a1]); }
int64_t yel_host_cli_variable(int64_t a1) { return ((int64_t)(uintptr_t)environ[a1]); }
int64_t yel_host_cli_working_directory(void) { return ((int64_t)(uintptr_t)getcwd(NULL, 0)); }
ystr yel_host_cli_c_string(int64_t a1) { return (y_str_of((const char *)(uintptr_t)(a1), (int64_t)strlen((const char *)(uintptr_t)(a1)))); }
yunit yel_host_cli_exit(int64_t a1) { return (y_out_flush(), exit((int)(a1)), (yunit)0); }
int64_t yel_host_clocks_realtime(void) { return ((int64_t)CLOCK_REALTIME); }
int64_t yel_host_clocks_monotonic(void) { return ((int64_t)CLOCK_MONOTONIC); }
yunit yel_host_clocks_get_time(int64_t a1, int64_t a2) { return (clock_gettime((clockid_t)(a1), (struct timespec *)(uintptr_t)(a2)), (yunit)0); }
yunit yel_host_clocks_get_resolution(int64_t a1, int64_t a2) { return (clock_getres((clockid_t)(a1), (struct timespec *)(uintptr_t)(a2)), (yunit)0); }
int64_t yel_host_filesystem_openat(int64_t a1, int64_t a2, int64_t a3) { return ((int64_t)openat((int)(a1), (const char *)(uintptr_t)(a2), (int)(a3), 0666)); }
int64_t yel_host_filesystem_close(int64_t a1) { return ((int64_t)close((int)(a1))); }
int64_t yel_host_filesystem_pread(int64_t a1, int64_t a2, int64_t a3, int64_t a4) { return ((int64_t)pread((int)(a1), (void *)(uintptr_t)(a2), (size_t)(a3), (off_t)(a4))); }
int64_t yel_host_filesystem_pwrite(int64_t a1, int64_t a2, int64_t a3, int64_t a4) { return ((int64_t)pwrite((int)(a1), (const void *)(uintptr_t)(a2), (size_t)(a3), (off_t)(a4))); }
int64_t yel_host_filesystem_seek_end(int64_t a1) { return ((int64_t)lseek((int)(a1), 0, SEEK_END)); }
int64_t yel_host_filesystem_mkdirat(int64_t a1, int64_t a2) { return ((int64_t)mkdirat((int)(a1), (const char *)(uintptr_t)(a2), 0777)); }
int64_t yel_host_filesystem_unlinkat(int64_t a1, int64_t a2, int64_t a3) { return ((int64_t)unlinkat((int)(a1), (const char *)(uintptr_t)(a2), (int)(a3))); }
int64_t yel_host_filesystem_renameat(int64_t a1, int64_t a2, int64_t a3, int64_t a4) { return ((int64_t)renameat((int)(a1), (const char *)(uintptr_t)(a2), (int)(a3), (const char *)(uintptr_t)(a4))); }
yunit yel_host_filesystem_dir_close(int64_t a1) { return (closedir((DIR *)(uintptr_t)(a1)), (yunit)0); }
int64_t yel_host_filesystem_errno(void) { return ((int64_t)errno); }
int64_t yel_host_filesystem_at_cwd(void) { return ((int64_t)AT_FDCWD); }
int64_t yel_host_filesystem_at_remove_dir(void) { return ((int64_t)AT_REMOVEDIR); }
int64_t yel_host_filesystem_o_read_only(void) { return ((int64_t)O_RDONLY); }
int64_t yel_host_filesystem_o_write_only(void) { return ((int64_t)O_WRONLY); }
int64_t yel_host_filesystem_o_read_write(void) { return ((int64_t)O_RDWR); }
int64_t yel_host_filesystem_o_create(void) { return ((int64_t)O_CREAT); }
int64_t yel_host_filesystem_o_directory(void) { return ((int64_t)O_DIRECTORY); }
int64_t yel_host_filesystem_o_exclusive(void) { return ((int64_t)O_EXCL); }
int64_t yel_host_filesystem_o_truncate(void) { return ((int64_t)O_TRUNC); }
int64_t yel_host_filesystem_o_no_follow(void) { return ((int64_t)O_NOFOLLOW); }
int64_t yel_host_filesystem_e_access(void) { return ((int64_t)EACCES); }
int64_t yel_host_filesystem_e_exists(void) { return ((int64_t)EEXIST); }
int64_t yel_host_filesystem_e_no_entry(void) { return ((int64_t)ENOENT); }
int64_t yel_host_filesystem_e_not_directory(void) { return ((int64_t)ENOTDIR); }
int64_t yel_host_filesystem_e_is_directory(void) { return ((int64_t)EISDIR); }
int64_t yel_host_filesystem_e_not_empty(void) { return ((int64_t)ENOTEMPTY); }
int64_t yel_host_filesystem_e_permission(void) { return ((int64_t)EPERM); }
int64_t yel_host_filesystem_e_invalid(void) { return ((int64_t)EINVAL); }
int64_t yel_host_filesystem_e_bad_descriptor(void) { return ((int64_t)EBADF); }
int64_t yel_host_filesystem_e_name_too_long(void) { return ((int64_t)ENAMETOOLONG); }
int64_t yel_host_filesystem_e_loop(void) { return ((int64_t)ELOOP); }
int64_t yel_host_filesystem_e_cross_device(void) { return ((int64_t)EXDEV); }
int64_t yel_host_filesystem_e_no_space(void) { return ((int64_t)ENOSPC); }
int64_t yel_host_filesystem_e_read_only(void) { return ((int64_t)EROFS); }
int64_t yel_host_filesystem_e_busy(void) { return ((int64_t)EBUSY); }
int64_t yel_host_random_fill(int64_t a1, int64_t a2) { return ((int64_t)getentropy((void *)(uintptr_t)(a1), (size_t)(a2))); }
int64_t yel_host_sockets_inet(void) { return ((int64_t)AF_INET); }
int64_t yel_host_sockets_inet6(void) { return ((int64_t)AF_INET6); }
int64_t yel_host_sockets_socket(int64_t a1) { return ((int64_t)socket((int)(a1), SOCK_STREAM, 0)); }
int64_t yel_host_sockets_e_not_supported(void) { return ((int64_t)EOPNOTSUPP); }
int64_t yel_host_sockets_e_family_not_supported(void) { return ((int64_t)EAFNOSUPPORT); }
int64_t yel_host_sockets_e_protocol_not_supported(void) { return ((int64_t)EPROTONOSUPPORT); }
int64_t yel_host_sockets_e_no_memory(void) { return ((int64_t)ENOMEM); }
int64_t yel_host_sockets_e_no_buffers(void) { return ((int64_t)ENOBUFS); }
int64_t yel_host_sockets_e_timed_out(void) { return ((int64_t)ETIMEDOUT); }
int64_t yel_host_sockets_e_connected(void) { return ((int64_t)EISCONN); }
int64_t yel_host_sockets_e_already(void) { return ((int64_t)EALREADY); }
int64_t yel_host_sockets_e_address_not_available(void) { return ((int64_t)EADDRNOTAVAIL); }
int64_t yel_host_sockets_e_address_in_use(void) { return ((int64_t)EADDRINUSE); }
int64_t yel_host_sockets_e_host_unreachable(void) { return ((int64_t)EHOSTUNREACH); }
int64_t yel_host_sockets_e_network_unreachable(void) { return ((int64_t)ENETUNREACH); }
int64_t yel_host_sockets_e_connection_refused(void) { return ((int64_t)ECONNREFUSED); }
int64_t yel_host_sockets_e_pipe(void) { return ((int64_t)EPIPE); }
int64_t yel_host_sockets_e_connection_reset(void) { return ((int64_t)ECONNRESET); }
int64_t yel_host_sockets_e_connection_aborted(void) { return ((int64_t)ECONNABORTED); }
int64_t yel_host_sockets_e_message_size(void) { return ((int64_t)EMSGSIZE); }
bool yel_host_terminal_is_terminal(int64_t a1) { return (isatty((int)(a1)) != 0); }
#endif

// std:process: a child process (fork), how one ended (waitpid), and a process's end as it is (no
// exit handlers: a child's, once its work is done). A target without processes (wasm) has no child:
// fork gives -1 there
#if defined(__wasm__)
int64_t yel_process_fork(void) { return -1; }
int64_t yel_process_run(ystr arguments, int64_t count, ystr dir, ystr environment, int64_t variables, ystr output) {
	(void)arguments;
	(void)count;
	(void)dir;
	(void)environment;
	(void)variables;
	(void)output;
	return -1;
}
int64_t yel_process_wait(int64_t child) {
	(void)child;
	return -1;
}
int64_t yel_process_start(ystr arguments, int64_t count, ystr dir, ystr environment, int64_t variables, ystr output) {
	(void)arguments;
	(void)count;
	(void)dir;
	(void)environment;
	(void)variables;
	(void)output;
	return 0;
}
bool yel_process_done(int64_t child) {
	(void)child;
	return true;
}
int64_t yel_process_status(int64_t child) {
	(void)child;
	return -1;
}
yunit yel_process_free(int64_t child) {
	(void)child;
	return 0;
}
ystr yel_build_entries(ystr dir) {
	(void)dir;
	return (ystr){ 0, "" };
}
bool yel_build_runnable(ystr at) {
	(void)at;
	return false;
}
int64_t yel_build_parallelism(void) { return 1; }
Y_NORETURN void yel_process_end(int64_t code) {
	y_out_flush();
	exit((int)code);
}
#else
#include <sys/wait.h>
/** A child (0 in the child, its pid in the parent, -1 for none), every output flushed first (so the child repeats none). */
int64_t yel_process_fork(void) {
	fflush(NULL);
	const pid_t child = fork();
	// the child's event loop made again: a kqueue (macOS, the BSDs) is not a child's, nor are the
	// loop's threads (libuv's thread pool starts again itself)
	if (child == 0) {
		y_forked = true;
		uv_loop_fork(uv_default_loop());
	}
	return (int64_t)child;
}
/** How a child ended: its exit status (0 to 255), 256 + the signal that ended it, or -1 (no such child). */
int64_t yel_process_wait(int64_t child) {
	int status = 0;
	if (waitpid((pid_t)child, &status, 0) < 0) return -1;
	if (WIFEXITED(status)) return (int64_t)WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 256 + (int64_t)WTERMSIG(status);
	return -1;
}
/** The process ends with code, its output flushed, and nothing more of it runs (_exit: no exit handlers). */
Y_NORETURN void yel_process_end(int64_t code) {
	fflush(NULL);
	_exit((int)code);
}
#include <fcntl.h>
#include <spawn.h>
extern char **environ;
/** count NUL-ended strings of joined (a copy of text's bytes), each an entry of a NULL-ended array. */
static char **y_split_nul(char *joined, int64_t length, int64_t count) {
	char **out = calloc((size_t)count + 1, sizeof(char *));
	int64_t at = 0;
	for (int64_t index = 0; index < count; index++) {
		out[index] = joined + at;
		while (at < length && joined[at] != 0) at++;
		at++;
	}
	out[count] = NULL;
	return out;
}

// a program's arguments and environment, as C has them: argv (count of them in arguments, each
// ended by a NUL byte), envp (this process's environment, those of environment's variables in place
// of one of their name, then the rest), and the strings they point into
typedef struct {
	char *joined, **argv, *given, **added, **envp;
} y_command;

static y_command y_command_of(ystr arguments, int64_t count, ystr environment, int64_t variables) {
	y_command c;
	c.joined = y_cstr(arguments);
	c.argv = y_split_nul(c.joined, arguments.len, count);
	c.given = y_cstr(environment);
	c.added = y_split_nul(c.given, environment.len, variables);
	int64_t own = 0;
	while (environ[own] != NULL) own++;
	c.envp = calloc((size_t)(own + variables) + 1, sizeof(char *));
	int64_t kept = 0;
	for (int64_t index = 0; index < own; index++) {
		const char *equals = strchr(environ[index], '=');
		const size_t name = equals ? (size_t)(equals - environ[index]) + 1 : strlen(environ[index]);
		bool replaced = false;
		for (int64_t other = 0; other < variables; other++) {
			if (strncmp(environ[index], c.added[other], name) == 0) replaced = true;
		}
		if (!replaced) c.envp[kept++] = environ[index];
	}
	for (int64_t other = 0; other < variables; other++) c.envp[kept++] = c.added[other];
	c.envp[kept] = NULL;
	return c;
}

static void y_command_free(y_command c) {
	free(c.envp);
	free(c.added);
	free(c.given);
	free(c.argv);
	free(c.joined);
}

/**
 * A program run, and waited for: arguments are its count arguments, the first its command (looked
 * for on PATH), each ended by a NUL byte; it runs in dir (empty: this process's), with this
 * process's environment and variables more (environment: NAME=value, each ended by a NUL byte, one
 * of a name this process has in place of its), its standard output and error to the file output
 * (made or emptied; empty: this process's). How it ended, as yel_process_wait tells it; -1 where it
 * could not be started.
 */
int64_t yel_process_run(ystr arguments, int64_t count, ystr dir, ystr environment, int64_t variables, ystr output) {
	if (count <= 0) return -1;
	y_command c = y_command_of(arguments, count, environment, variables);
	fflush(NULL);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	char *to = NULL;
	if (output.len > 0) {
		to = y_cstr(output);
		posix_spawn_file_actions_addopen(&actions, 1, to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		posix_spawn_file_actions_adddup2(&actions, 1, 2);
	}
	char *in = NULL;
	if (dir.len > 0) {
		in = y_cstr(dir);
		// (POSIX 2024's name where the system is that new: macOS 26 deprecates the _np one)
#if defined(__APPLE__) && defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
		posix_spawn_file_actions_addchdir(&actions, in);
#else
		posix_spawn_file_actions_addchdir_np(&actions, in);
#endif
	}
	pid_t child = 0;
	const int started = posix_spawnp(&child, c.argv[0], &actions, NULL, c.argv, c.envp);
	posix_spawn_file_actions_destroy(&actions);
	free(to);
	free(in);
	y_command_free(c);
	if (started != 0) return -1;
	return yel_process_wait((int64_t)child);
}

// a child started on the loop (yel_process_start's): the task that waits for it, and how it ended
typedef struct y_child {
	uv_process_t process;
	y_async *task;
	int64_t status;
	bool done;
	struct y_child *next;
} y_child;

// every child not let go yet, so the collector keeps the tasks they wake
static y_child *y_children;

static void y_trace_children(void) {
	for (y_child *c = y_children; c; c = c->next) y_mark(c->task);
}

static void y_child_exited(uv_process_t *process, int64_t status, int signal) {
	y_child *c = process->data;
	c->status = signal ? 256 + signal : status;
	c->done = true;
	y_uv_pending--;
	y_wake(c->task);
}

static void y_child_closed(uv_handle_t *handle) { free(handle->data); }

/**
 * A program started as yel_process_run runs one, without waiting for it: the child, which the
 * task parks for until yel_process_done (how it ended then yel_process_status, as
 * yel_process_wait tells it, and the child let go with yel_process_free); 0 where it could not be
 * started.
 */
int64_t yel_process_start(ystr arguments, int64_t count, ystr dir, ystr environment, int64_t variables, ystr output) {
	static bool traced;
	if (!traced) {
		y_tracer(y_trace_children);
		traced = true;
	}
	if (count <= 0) return 0;
	y_command c = y_command_of(arguments, count, environment, variables);
	int out = -1;
	if (output.len > 0) {
		char *to = y_cstr(output);
		out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		free(to);
		if (out < 0) {
			y_command_free(c);
			return 0;
		}
	}
	uv_stdio_container_t stdio[3];
	stdio[0].flags = UV_INHERIT_FD;
	stdio[0].data.fd = 0;
	for (int index = 1; index < 3; index++) {
		stdio[index].flags = UV_INHERIT_FD;
		stdio[index].data.fd = out >= 0 ? out : index;
	}
	char *in = dir.len > 0 ? y_cstr(dir) : NULL;
	uv_process_options_t options;
	memset(&options, 0, sizeof options);
	options.file = c.argv[0];
	options.args = c.argv;
	options.env = c.envp;
	options.cwd = in;
	options.stdio_count = 3;
	options.stdio = stdio;
	options.exit_cb = y_child_exited;
	y_child *child = calloc(1, sizeof(y_child));
	if (!child) y_die("out of memory");
	child->process.data = child;
	child->task = y_async_now;
	fflush(NULL);
	const int started = uv_spawn(uv_default_loop(), &child->process, &options);
	if (out >= 0) close(out);
	free(in);
	y_command_free(c);
	if (started != 0) {
		// (a handle uv_spawn failed for is still closed)
		uv_close((uv_handle_t *)&child->process, y_child_closed);
		return 0;
	}
	y_trace_waits("a child process");
	y_uv_pending++;
	child->next = y_children;
	y_children = child;
	return (int64_t)(uintptr_t)child;
}

// std:build's, as a build program declares (no task to park: it is not running yet)

/** dir's names (not . nor ..), each ended by a newline, a directory's by "/" and a newline; "" where it cannot be read. */
ystr yel_build_entries(ystr dir) {
	char *at = y_cstr(dir);
	DIR *opened = opendir(at);
	if (!opened) {
		free(at);
		return (ystr){ 0, "" };
	}
	size_t length = 0, capacity = 256;
	char *out = malloc(capacity);
	for (struct dirent *entry = readdir(opened); entry; entry = readdir(opened)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		const size_t name = strlen(entry->d_name);
		char *full = malloc(strlen(at) + name + 2);
		sprintf(full, "%s/%s", at, entry->d_name);
		struct stat st;
		const bool directory = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
		free(full);
		while (length + name + 2 > capacity) out = realloc(out, capacity *= 2);
		if (length > 0) out[length++] = '\n';
		memcpy(out + length, entry->d_name, name);
		length += name;
		if (directory) out[length++] = '/';
	}
	closedir(opened);
	free(at);
	const ystr names = y_str_of(out, (int64_t)length);
	free(out);
	return names;
}

/** Whether at is a file that can be run. */
bool yel_build_runnable(ystr at) {
	char *path = y_cstr(at);
	struct stat st;
	const bool runnable = stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
	free(path);
	return runnable;
}

/** How many programs can usefully run at once: the system's cores (libuv's count). */
int64_t yel_build_parallelism(void) { return (int64_t)uv_available_parallelism(); }

bool yel_process_done(int64_t child) { return ((y_child *)(uintptr_t)child)->done; }
int64_t yel_process_status(int64_t child) { return ((y_child *)(uintptr_t)child)->status; }

yunit yel_process_free(int64_t child) {
	y_child *done = (y_child *)(uintptr_t)child;
	for (y_child **at = &y_children; *at; at = &(*at)->next) {
		if (*at == done) {
			*at = done->next;
			break;
		}
	}
	uv_close((uv_handle_t *)&done->process, y_child_closed);
	return 0;
}
#endif
