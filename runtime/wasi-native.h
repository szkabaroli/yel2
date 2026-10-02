// WASI 0.3 natively: what std's own host (runtime/std/host-*.yel: cli, clocks, filesystem, random,
// sockets, terminals) cannot say in yel, the system's structs read into numbers. A program's call of
// an import std's host has no func for stops at its C compile with a message instead.
#ifndef YEL_WASI_NATIVE_H
#define YEL_WASI_NATIVE_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// the environment, for std's wasi:cli host (runtime/std/host-cli.yel)
extern char **environ;

// ---- wasi:filesystem natively: what std's host (runtime/std/host-filesystem.yel) cannot say in yel,
// the system's structs read into numbers. A descriptor's handle there is its fd + 1

#include <dirent.h>
#include <sys/stat.h>

// a kind of file as the host numbers it: 0 regular, 1 directory, 2 symbolic link, 3 block device,
// 4 character device, 5 fifo, 6 socket, 7 other
int64_t yn_fs_kind(mode_t m);

// fd's stat (path ""), or that of path in fd's directory, into out: kind, link count, size, then
// the access, modification and status change times (seconds, nanoseconds each); 0, or -errno
int64_t yn_fs_stat(int64_t fd, ystr path, bool follow, int64_t out);

// fd's directory opened for its entries (its DIR as a number: over a dup, so closing it leaves fd);
// -errno where it cannot be
int64_t yn_fs_dir_open(int64_t fd);

// the next entry's name ("" at the end; . and .. passed over) and its kind (as yn_fs_kind's, into
// *kind)
ystr yn_fs_dir_next(int64_t dir, int64_t kind);

// ---- wasi:sockets natively: what std's host (runtime/std/host-sockets.yel) cannot say in yel, a
// socket address as numbers (12 of them): its family (0 ipv4, 1 ipv6), port, flow info and scope
// id, then the address's 4 bytes or 8 segments

// fd bound to the address the numbers at say (the address reusable at once); 0, or -errno
int64_t yn_socket_bind(int64_t fd, int64_t at);

// fd's local address as numbers, written at (before it is bound: its family, port 0); 0, or -errno
int64_t yn_socket_name(int64_t fd, int64_t at);

// std's native hosts' calls of the system (runtime/std/host-*.yel's externs, each yel_<file>_<name>):
// a constant, a system call, a C library function, as its numbers
int64_t yel_host_cli_argument_count(void);
int64_t yel_host_cli_argument(int64_t a1);
int64_t yel_host_cli_variable(int64_t a1);
int64_t yel_host_cli_working_directory(void);
ystr yel_host_cli_c_string(int64_t a1);
yunit yel_host_cli_exit(int64_t a1);
int64_t yel_host_clocks_realtime(void);
int64_t yel_host_clocks_monotonic(void);
yunit yel_host_clocks_get_time(int64_t a1, int64_t a2);
yunit yel_host_clocks_get_resolution(int64_t a1, int64_t a2);
int64_t yel_host_filesystem_openat(int64_t a1, int64_t a2, int64_t a3);
int64_t yel_host_filesystem_close(int64_t a1);
int64_t yel_host_filesystem_pread(int64_t a1, int64_t a2, int64_t a3, int64_t a4);
int64_t yel_host_filesystem_pwrite(int64_t a1, int64_t a2, int64_t a3, int64_t a4);
int64_t yel_host_filesystem_seek_end(int64_t a1);
int64_t yel_host_filesystem_mkdirat(int64_t a1, int64_t a2);
int64_t yel_host_filesystem_unlinkat(int64_t a1, int64_t a2, int64_t a3);
int64_t yel_host_filesystem_renameat(int64_t a1, int64_t a2, int64_t a3, int64_t a4);
yunit yel_host_filesystem_dir_close(int64_t a1);
int64_t yel_host_filesystem_errno(void);
int64_t yel_host_filesystem_at_cwd(void);
int64_t yel_host_filesystem_at_remove_dir(void);
int64_t yel_host_filesystem_o_read_only(void);
int64_t yel_host_filesystem_o_write_only(void);
int64_t yel_host_filesystem_o_read_write(void);
int64_t yel_host_filesystem_o_create(void);
int64_t yel_host_filesystem_o_directory(void);
int64_t yel_host_filesystem_o_exclusive(void);
int64_t yel_host_filesystem_o_truncate(void);
int64_t yel_host_filesystem_o_no_follow(void);
int64_t yel_host_filesystem_e_access(void);
int64_t yel_host_filesystem_e_exists(void);
int64_t yel_host_filesystem_e_no_entry(void);
int64_t yel_host_filesystem_e_not_directory(void);
int64_t yel_host_filesystem_e_is_directory(void);
int64_t yel_host_filesystem_e_not_empty(void);
int64_t yel_host_filesystem_e_permission(void);
int64_t yel_host_filesystem_e_invalid(void);
int64_t yel_host_filesystem_e_bad_descriptor(void);
int64_t yel_host_filesystem_e_name_too_long(void);
int64_t yel_host_filesystem_e_loop(void);
int64_t yel_host_filesystem_e_cross_device(void);
int64_t yel_host_filesystem_e_no_space(void);
int64_t yel_host_filesystem_e_read_only(void);
int64_t yel_host_filesystem_e_busy(void);
int64_t yel_host_random_fill(int64_t a1, int64_t a2);
int64_t yel_host_sockets_inet(void);
int64_t yel_host_sockets_inet6(void);
int64_t yel_host_sockets_socket(int64_t a1);
yunit yel_host_sockets_accepting(int64_t a1, int64_t a2);
int64_t yel_host_sockets_e_not_supported(void);
int64_t yel_host_sockets_e_family_not_supported(void);
int64_t yel_host_sockets_e_protocol_not_supported(void);
int64_t yel_host_sockets_e_no_memory(void);
int64_t yel_host_sockets_e_no_buffers(void);
int64_t yel_host_sockets_e_timed_out(void);
int64_t yel_host_sockets_e_connected(void);
int64_t yel_host_sockets_e_already(void);
int64_t yel_host_sockets_e_address_not_available(void);
int64_t yel_host_sockets_e_address_in_use(void);
int64_t yel_host_sockets_e_host_unreachable(void);
int64_t yel_host_sockets_e_network_unreachable(void);
int64_t yel_host_sockets_e_connection_refused(void);
int64_t yel_host_sockets_e_pipe(void);
int64_t yel_host_sockets_e_connection_reset(void);
int64_t yel_host_sockets_e_connection_aborted(void);
int64_t yel_host_sockets_e_message_size(void);
bool yel_host_terminal_is_terminal(int64_t a1);

#endif
