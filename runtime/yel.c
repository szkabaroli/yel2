// The runtime's definitions (yel.h declares them): compiled once for a target and linked with
// each program (the C backend's, the bitcode backend's alike).
#include "yel.h"

Y_NORETURN void y_die(const char *message) {
	fflush(stdout);
	fprintf(stderr, "yel: %s\n", message);
	exit(1);
}

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
// y_heap_min (YEL_GC_MIN_MB, default 4, as Go's): a program that keeps most of what it makes (a compile)
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
	if (y_verbose)
		fprintf(stderr, "yel gc: %.1f MiB -> %.1f MiB live, next at x%d\n", (double)y_heap_bytes / 1048576.0,
			(double)live / 1048576.0, (int)grow);
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
	if (!y_gc_hold && (y_stress ? --y_stress_left <= 0 : y_heap_bytes >= y_heap_limit)) {
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

// a float's text, the shortest that reads back as it (nan, inf, -inf): std's format.write-f64
ystr yel_f64_text(double v) {
	if (isnan(v)) return y_str_of("nan", 3);
	if (isinf(v)) return v > 0 ? y_str_of("inf", 3) : y_str_of("-inf", 4);
	char tmp[40];
	for (int digits = 1; digits <= 17; digits++) {
		snprintf(tmp, sizeof tmp, "%.*g", digits, v);
		if (strtod(tmp, NULL) == v) break;
	}
	return y_str_of(tmp, (int64_t)strlen(tmp));
}

ystr yel_f32_text(float v) {
	if (isnan(v) || isinf(v)) return yel_f64_text(v);
	char tmp[40];
	for (int digits = 1; digits <= 9; digits++) {
		snprintf(tmp, sizeof tmp, "%.*g", digits, (double)v);
		if (strtof(tmp, NULL) == v) break;
	}
	return y_str_of(tmp, (int64_t)strlen(tmp));
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
	const char *no = getenv("NO_COLOR");
	return (no == NULL || no[0] == 0) && isatty(2);
}

// a match no arm fit: the program stops, showing the value
Y_NORETURN void yel_no_match(ystr shown) {
	fflush(stdout);
	fprintf(stderr, "yel: no match arm for %.*s\n", (int)shown.len, shown.data);
	exit(1);
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
	int len = snprintf(NULL, 0, "%.*f", digits, value);
	char *text = malloc((size_t)len + 1);
	if (!text) y_die("out of memory");
	snprintf(text, (size_t)len + 1, "%.*f", digits, value);
	ystr out = y_str_of(text, len);
	free(text);
	return out;
}

// ---------------------------------------------------------------- the process

ylist *y_args;

Y_NORETURN void y_index_out_of_range(int64_t i, int64_t n) {
	char message[96];
	snprintf(message, sizeof message, "index %lld out of range (length %lld)", (long long)i, (long long)n);
	y_die(message);
}

yunit yel_print(ystr s) {
	fwrite(s.data, 1, (size_t)s.len, stdout);
	return 0;
}

yunit yel_eprint(ystr s) {
	fwrite(s.data, 1, (size_t)s.len, stderr);
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

/** What main does last: stdout flushed, main's value the exit code. */
int y_finish(int64_t code) {
	fflush(stdout);
	return (int)code;
}

Y_NORETURN yunit yel_panic(ystr message) {
	fflush(stdout);
	fputs("panic: ", stderr);
	yel_eprint(message);
	fputc('\n', stderr);
	exit(1);
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

/** A string the host wrote (into memory from cabi_realloc): copied to the heap, the host's freed. */
void y_flush_stdout(void) { fflush(stdout); }

Y_NORETURN void y_abi_bad(int32_t what) {
	static const char *const said[] = { "a value outside its type crossed the boundary", "a case the union does not have crossed the boundary", "a case the variant does not have", "a case the enum does not have crossed the boundary", "a char that is not a Unicode scalar value crossed the boundary", "a tag out of its type's range crossed the boundary", "a future's writer was dropped before it wrote" };
	y_die(said[what < 0 || what > 6 ? 0 : what]);
}

ystr y_abi_lift_str(yabi_str *s) {
	ystr out = y_str_of(s->ptr, (int64_t)s->len);
	free(s->ptr);
	return out;
}

#if defined(__wasm__)
// what the host writes into the component's memory (a string, a list) comes from here: plain
// memory, which lifting copies to the heap and frees
__attribute__((export_name("cabi_realloc"))) void *cabi_realloc(void *old, size_t old_size, size_t align, size_t new_size) {
	(void)old_size;
	(void)align;
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

// a turn's end (an export's post-return: nothing of the component is on the stack): the
// collector runs when the heap has grown past half of what starts one
void y_turn_end(void) {
#ifndef Y_NO_GC
	if (!y_stress && y_heap_bytes >= y_heap_limit / 2) y_collect();
#endif
}

// a count from the environment: a decimal, 0 for anything else (no overflow: strtol clamps)
long y_env_count(const char *s) {
	char *end;
	const long n = strtol(s, &end, 10);
	return n < 0 ? 0 : n;
}

/** The program's arguments (argv after the program), for main. */
ylist *y_start(int argc, char **argv) {
	y_started = true;
	y_argc = argc;
	y_argv = argv;
	const char *stress = getenv("YEL_GC_STRESS");
	if (stress) y_stress = y_stress_left = y_env_count(stress);
	const char *stats = getenv("YEL_GC_STATS");
	if (stats) {
		atexit(y_stats);
		// YEL_GC_STATS=2: each collection too
		y_verbose = y_env_count(stats) > 1;
	}
	const char *min = getenv("YEL_GC_MIN_MB");
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

/** A task back in the ready queue (once; not once done). */
void y_wake(y_async *t) {
	if (!t || t->done || t->queued) return;
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

/** A task started: its own, the executor's to step (ready at once); its owner the starter's (a
root's: itself). */
y_async *yel_async_spawn(y_async *t) {
	// rooted while the task list grows (the caller may hold it nowhere else)
	Y_FRAME(1);
	ys_[0] = t;
	t->spawned = 1;
	t->owner = y_async_now && y_async_now->owner ? y_async_now->owner : t;
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

/** A task stepped by the executor (the one a wait in it parks); done: its waiters woken. */
yunit yel_async_run(y_async *t) {
	if (t->done) return 0;
	y_async *was = y_async_now;
	y_async_now = t;
	if (t->step(t)) {
		t->done = 1;
		t->order = ++y_async_order;
		y_async_finished(t);
	}
	y_async_now = was;
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
			return 0;
		}
		break;
	}
	if (y_async_host_pending(root)) return (int32_t)(2u | (y_host_root(root)->set << 4));
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

// ---- wasi:sockets natively: what std's host (runtime/std/host-sockets.yel) cannot say in yel, a
// socket address as numbers (12 of them): its family (0 ipv4, 1 ipv6), port, flow info and scope
// id, then the address's 4 bytes or 8 segments

// fd bound to the address the numbers at say (the address reusable at once); 0, or -errno
int64_t yn_socket_bind(int64_t fd, int64_t at) {
  const int64_t *parts = (const int64_t *)(uintptr_t)at;
  struct sockaddr_storage storage = {0};
  socklen_t length;
  if (parts[0] == 0) {
    struct sockaddr_in *address = (struct sockaddr_in *)&storage;
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)parts[1]);
    uint8_t *bytes = (uint8_t *)&address->sin_addr;
    for (int index = 0; index < 4; index++)
      bytes[index] = (uint8_t)parts[4 + index];
    length = sizeof *address;
  } else {
    struct sockaddr_in6 *address = (struct sockaddr_in6 *)&storage;
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
  int one = 1;
  setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  return bind((int)fd, (struct sockaddr *)&storage, length) < 0 ? -errno : 0;
}

// fd's local address as numbers, written at (before it is bound: its family, port 0); 0, or -errno
int64_t yn_socket_name(int64_t fd, int64_t at) {
  int64_t *parts = (int64_t *)(uintptr_t)at;
  struct sockaddr_storage storage = {0};
  socklen_t length = sizeof storage;
  if (getsockname((int)fd, (struct sockaddr *)&storage, &length) < 0)
    return -errno;
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
  return 0;
}

// ---- std's native hosts' calls of the system (wasi-native.h declares them)
int64_t yel_host_cli_argument_count(void) { return ((int64_t)y_argc); }
int64_t yel_host_cli_argument(int64_t a1) { return ((int64_t)(uintptr_t)y_argv[a1]); }
int64_t yel_host_cli_variable(int64_t a1) { return ((int64_t)(uintptr_t)environ[a1]); }
int64_t yel_host_cli_working_directory(void) { return ((int64_t)(uintptr_t)getcwd(NULL, 0)); }
ystr yel_host_cli_c_string(int64_t a1) { return (y_str_of((const char *)(uintptr_t)(a1), (int64_t)strlen((const char *)(uintptr_t)(a1)))); }
yunit yel_host_cli_exit(int64_t a1) { return (fflush(stdout), exit((int)(a1)), (yunit)0); }
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
yunit yel_host_sockets_accepting(int64_t a1, int64_t a2) { return (getsockopt((int)(a1), SOL_SOCKET, SO_ACCEPTCONN, (void *)(uintptr_t)(a2), &(socklen_t){ sizeof(int) }), (yunit)0); }
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
