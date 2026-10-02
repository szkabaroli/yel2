// C functions tests/bitcode/abi.yel calls: each shape of value crossing a C call (a struct in
// registers, in parts, in memory; a struct result; narrow integers widened), built with the program
// (tools/bitcode-test.sh: included into the C build, compiled to bitcode for the bitcode one).
#include "yel.h"

// a string given and given back (arm64: [2 x i64] both ways; x86-64: two registers, a { i64, ptr }
// back; wasm32: byval, and sret)
ystr abi_echo(ystr s) { return s; }

// narrow integers (each widened by the caller: zeroext, signext) and a bool
int64_t abi_narrow(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
	return (int64_t)a * 10000 + (int64_t)b * 1000 + (int64_t)c * 100 + (int64_t)d * 10 + (e ? 1 : 0);
}

// narrow results (the callee widens them)
bool abi_odd(int32_t x) { return (x & 1) != 0; }

int8_t abi_negate(int8_t x) { return (int8_t)-x; }

uint16_t abi_wide(uint16_t x) { return (uint16_t)(x * 2); }

// more strings than x86-64 has registers for (6 integer registers: the third string on is passed
// in memory), and the last of them back
ystr abi_fourth(ystr a, ystr b, ystr c, ystr d) {
	(void)a;
	(void)b;
	(void)c;
	return d;
}

int64_t abi_lengths(ystr a, ystr b, ystr c, ystr d) { return a.len + b.len * 10 + c.len * 100 + d.len * 1000; }

// scalars and strings mixed (a register left over: an s64 after two strings)
int64_t abi_mixed(int64_t a, ystr s, int32_t b, ystr t, uint8_t c) { return a + s.len * 10 + b * 100 + t.len * 1000 + c * 10000; }
