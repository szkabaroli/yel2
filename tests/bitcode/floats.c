// C functions tests/bitcode/floats.yel calls: structs holding floats, by value both ways, laid out
// as yel's value structs are (a tuple's fields in order, an option's has then its value, a record of
// fields by its fields' names). The bitcode build only (tools/bitcode-test.sh: tests/bitcode/floats.out
// says what it prints): C cannot name yel's own struct types to share them with a C build.
#include <stdbool.h>
#include <stdint.h>

// two doubles: arm64's floats in two registers, x86-64's two SSE eightbytes, wasm32's in memory
typedef struct { double a, b; } d2;
d2 floats_swap(d2 p) { return (d2){ p.b, p.a }; }

// five of them: more than the float registers hold (the last ones in memory)
double floats_sum5(d2 a, d2 b, d2 c, d2 d, d2 e) { return a.a + a.b * 2 + b.a * 4 + b.b * 8 + c.a * 16 + c.b * 32 + d.a * 64 + d.b * 128 + e.a * 256 + e.b * 512; }

// three floats: arm64's three float registers; x86-64's first eightbyte two of them
typedef struct { float a, b, c; } f3;
f3 floats_scale(f3 v, float k) { return (f3){ v.a * k, v.b * k, v.c * k }; }

// a double and an integer: an SSE and an INTEGER eightbyte (x86-64); [2 x i64] (arm64)
typedef struct { double a; int64_t b; } di;
di floats_mix(di v) { return (di){ v.a * 2, v.b + 1 }; }

// an integer and a float in one eightbyte: an INTEGER one (x86-64)
typedef struct { int32_t a; float b; } i_f;
i_f floats_pair(i_f v) { return (i_f){ v.a * 3, v.b + 0.5f }; }

// five doubles: 40 bytes, in memory both ways
typedef struct { double a, b, c, d, e; } d5;
d5 floats_reverse(d5 v) { return (d5){ v.e, v.d, v.c, v.b, v.a }; }

// an option<f64>
typedef struct { bool has; double value; } od;
od floats_half(od v) { return v.has ? (od){ true, v.value / 2 } : (od){ false, 0 }; }

// a record of one field (wasm32: as that double)
typedef struct { double x; } rec;
rec floats_negate(rec r) { return (rec){ -r.x }; }

// a struct in a struct (the inner one's padding kept)
typedef struct { struct { float a; int32_t b; } inner; double c; } nested;
nested floats_nested(nested n) { return (nested){ { n.inner.a * 2, n.inner.b * 2 }, n.c + n.inner.a }; }
