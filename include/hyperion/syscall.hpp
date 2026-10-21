// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest Linux system-call layer (Phase 6, increment 1).
//
// The x86-64 `syscall` instruction (0F 05) transfers control to the kernel with
// the number in rax and arguments in rdi, rsi, rdx, r10, r8, r9. Hyperion runs
// guest code as an ordinary unprivileged host process, so there is no real
// kernel transition: the instruction is routed to a SyscallHandler that
// emulates the call in userspace and writes the result back into rax.
//
// Scope of this increment is deliberately tiny and stated rather than implied:
//   write(2), read(2), close(2), lseek(2), openat(2), fstat(2), exit(2),
//   exit_group(2), the memory syscalls mmap(2), mprotect(2), munmap(2), brk(2),
//   the process-identity getters getpid(2), gettid(2), getuid(2), geteuid(2),
//   getgid(2), getegid(2), getppid(2), getcwd(2), and getrandom(2), the
//   path-metadata call newfstatat(2), and the path queries access(2) and
//   readlink(2).
// Every other syscall number returns -ENOSYS explicitly. Nothing silently
// succeeds. openat() is limited to AT_FDCWD and a guest pathname; fstat() and
// newfstatat() write the *guest's* (x86-64) struct stat layout, not the host's.
// The identity getters forward to the host process (Hyperion runs the guest
// in-process, so the guest sees the host's ids). The full inventory and the
// implementation order live in docs/syscalls/.
//
// Isolation: this is not a sandbox. openat() forwards to the host kernel, so the
// guest can open whatever the *host process* may open, exactly as SECURITY.md
// states. Run untrusted guests only inside an external isolation boundary.
//
// Error convention: a failed syscall returns a *negative* errno in rax (e.g.
// -EFAULT), exactly as the Linux kernel does; it does not return -1 and set a
// host errno. This is what guest programs test for.
#ifndef HYPERION_SYSCALL_HPP
#define HYPERION_SYSCALL_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "hyperion/address_space.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp" // Memory

namespace hyperion {

// x86-64 syscall numbers used by this increment. The authoritative list is
// docs/syscalls/kernel-table-x86_64-6.12.tsv; these are repeated here so the
// handler does not depend on the documentation.
namespace sysno {
inline constexpr std::uint64_t kRead = 0;
inline constexpr std::uint64_t kWrite = 1;
inline constexpr std::uint64_t kClose = 3;
inline constexpr std::uint64_t kFstat = 5;
inline constexpr std::uint64_t kLseek = 8;
inline constexpr std::uint64_t kMmap = 9;
inline constexpr std::uint64_t kMprotect = 10;
inline constexpr std::uint64_t kMunmap = 11;
inline constexpr std::uint64_t kBrk = 12;
inline constexpr std::uint64_t kPread64 = 17;
inline constexpr std::uint64_t kPwrite64 = 18;
inline constexpr std::uint64_t kReadv = 19;
inline constexpr std::uint64_t kWritev = 20;
inline constexpr std::uint64_t kAccess = 21;
inline constexpr std::uint64_t kDup = 32;
inline constexpr std::uint64_t kDup2 = 33;
inline constexpr std::uint64_t kGetpid = 39;
inline constexpr std::uint64_t kExit = 60;
inline constexpr std::uint64_t kUname = 63;
inline constexpr std::uint64_t kFcntl = 72;
inline constexpr std::uint64_t kGetcwd = 79;
inline constexpr std::uint64_t kReadlink = 89;
inline constexpr std::uint64_t kGettimeofday = 96;
inline constexpr std::uint64_t kGetuid = 102;
inline constexpr std::uint64_t kGetgid = 104;
inline constexpr std::uint64_t kGeteuid = 107;
inline constexpr std::uint64_t kGetegid = 108;
inline constexpr std::uint64_t kGetppid = 110;
inline constexpr std::uint64_t kArchPrctl = 158;
inline constexpr std::uint64_t kGettid = 186;
inline constexpr std::uint64_t kGetdents64 = 217;
inline constexpr std::uint64_t kSetTidAddress = 218;
inline constexpr std::uint64_t kClockGettime = 228;
inline constexpr std::uint64_t kExitGroup = 231;
inline constexpr std::uint64_t kOpenat = 257;
inline constexpr std::uint64_t kNewfstatat = 262;
inline constexpr std::uint64_t kReadlinkat = 267;
inline constexpr std::uint64_t kSetRobustList = 273;
inline constexpr std::uint64_t kDup3 = 292;
inline constexpr std::uint64_t kPrlimit64 = 302;
inline constexpr std::uint64_t kGetrandom = 318;
inline constexpr std::uint64_t kStatx = 332;
} // namespace sysno

namespace arch_prctl_code {
inline constexpr std::uint64_t kSetGs = 0x1001;
inline constexpr std::uint64_t kSetFs = 0x1002;
inline constexpr std::uint64_t kGetFs = 0x1003;
inline constexpr std::uint64_t kGetGs = 0x1004;
} // namespace arch_prctl_code

namespace fcntl_cmd {
inline constexpr std::int32_t kDupfd = 0;
inline constexpr std::int32_t kGetfd = 1;
inline constexpr std::int32_t kSetfd = 2;
inline constexpr std::int32_t kGetfl = 3;
inline constexpr std::int32_t kSetfl = 4;
inline constexpr std::int32_t kDupfdCloexec = 1030;
} // namespace fcntl_cmd

// openat(2) flags (the subset this increment maps to the host). The values match
// the x86-64 Linux ABI; a guest passing them gets the kernel's interpretation.
namespace open_flags {
inline constexpr std::uint64_t kOAccmode = 0x3; // mask of the low access bits
inline constexpr std::uint64_t kORdonly = 0x0;
inline constexpr std::uint64_t kOWronly = 0x1;
inline constexpr std::uint64_t kORdwr = 0x2;
inline constexpr std::uint64_t kOCreat = 0x40;
inline constexpr std::uint64_t kOTrunc = 0x200;
inline constexpr std::uint64_t kOAppend = 0x400;
inline constexpr std::uint64_t kOExcl = 0x80;
// O_DIRECTORY: fail unless the path names a directory. Its *guest* value
// (0x10000) differs from the host's on AArch64 (0x4000), so it is translated
// explicitly rather than passed through.
inline constexpr std::uint64_t kODirectory = 0x10000;
// O_NONBLOCK: non-blocking I/O. Same value on both ABIs, but translated
// explicitly for the same reason.
inline constexpr std::uint64_t kONonblock = 0x800;
// O_CLOEXEC: accepted and ignored; the handler always opens with O_CLOEXEC so a
// guest descriptor never leaks into a host child.
inline constexpr std::uint64_t kOCloseexec = 0x80000;
} // namespace open_flags

// AT_FDCWD: a relative pathname is resolved against the current working
// directory. Only this value is supported; any other dirfd returns -ENOSYS
// (a real *at() with an explicit directory descriptor is not implemented).
inline constexpr std::uint64_t kAtFdcwd = static_cast<std::uint64_t>(-100);

// AT_SYMLINK_NOFOLLOW: do not dereference the final path component (lstat).
inline constexpr std::uint64_t kAtSymlinkNofollow = 0x100;

// statx(2) flags and the request/return mask. The values are the same on x86-64
// and AArch64 (a fixed UAPI), so they are translated as-is. Only
// AT_SYMLINK_NOFOLLOW is honoured among the flags; the mask is honoured by the
// host, which fills exactly the fields the guest asked for.
namespace statx {
inline constexpr std::uint64_t kAtEmptyPath = 0x1000;
inline constexpr std::uint64_t kBasicStats = 0x7ff; // STATX_BASIC_STATS
} // namespace statx

// Guest (x86-64) `struct statx` field offsets. Unlike `struct stat`, this
// structure is built entirely from fixed-width `__u32`/`__u64` fields (the
// kernel's UAPI forbids padding differences), so the layout is *identical* on
// x86-64 and AArch64 and the host's bytes can be forwarded verbatim. The
// offsets are recorded here so a test can assert them against the UAPI, and so
// the writer is explicit rather than an unchecked struct copy.
namespace guest_statx {
inline constexpr std::size_t kSize = 256;         // sizeof(struct statx)
inline constexpr std::size_t kOffMask = 0;        // __u32 stx_mask
inline constexpr std::size_t kOffBlksize = 4;     // __u32 stx_blksize
inline constexpr std::size_t kOffAttributes = 8;  // __u64 stx_attributes
inline constexpr std::size_t kOffNlink = 16;      // __u32 stx_nlink
inline constexpr std::size_t kOffUid = 20;        // __u32 stx_uid
inline constexpr std::size_t kOffGid = 24;        // __u32 stx_gid
inline constexpr std::size_t kOffMode = 28;       // __u16 stx_mode (+1 pad)
inline constexpr std::size_t kOffIno = 32;        // __u64 stx_ino
inline constexpr std::size_t kOffSize = 40;       // __u64 stx_size
inline constexpr std::size_t kOffBlocks = 48;     // __u64 stx_blocks
inline constexpr std::size_t kOffAttrmask = 56;   // __u64 stx_attributes_mask
inline constexpr std::size_t kOffAtime = 64;      // struct statx_timestamp
inline constexpr std::size_t kOffBtime = 80;      // struct statx_timestamp
inline constexpr std::size_t kOffCtime = 96;      // struct statx_timestamp
inline constexpr std::size_t kOffMtime = 112;     // struct statx_timestamp
inline constexpr std::size_t kOffRdevMajor = 128; // __u32 stx_rdev_major
inline constexpr std::size_t kOffRdevMinor = 132; // __u32 stx_rdev_minor
inline constexpr std::size_t kOffDevMajor = 136;  // __u32 stx_dev_major
inline constexpr std::size_t kOffDevMinor = 140;  // __u32 stx_dev_minor
} // namespace guest_statx

// Guest (x86-64) `struct stat` field offsets. The host layout differs on
// AArch64, so fstat() builds the guest layout field by field rather than
// copying the host struct. Offsets are for the x86-64 LP64 layout.
namespace guest_stat {
inline constexpr std::size_t kSize = 144;        // sizeof(struct stat) on x86-64
inline constexpr std::size_t kOffStDev = 0;      // dev_t st_dev (8)
inline constexpr std::size_t kOffStIno = 8;      // ino_t st_ino (8)
inline constexpr std::size_t kOffStNlink = 16;   // nlink_t st_nlink (8)
inline constexpr std::size_t kOffStMode = 24;    // mode_t st_mode (4)
inline constexpr std::size_t kOffStUid = 28;     // uid_t st_uid (4)
inline constexpr std::size_t kOffStGid = 32;     // gid_t st_gid (4)
inline constexpr std::size_t kOffStRdev = 40;    // dev_t st_rdev (8)
inline constexpr std::size_t kOffStSize = 48;    // off_t st_size (8)
inline constexpr std::size_t kOffStBlksize = 56; // blksize_t st_blksize (8)
inline constexpr std::size_t kOffStBlocks = 64;  // blkcnt_t st_blocks (8)
inline constexpr std::size_t kOffStAtime = 72;   // struct timespec st_atim (16)
inline constexpr std::size_t kOffStMtime = 88;   // struct timespec st_mtim (16)
inline constexpr std::size_t kOffStCtime = 104;  // struct timespec st_ctim (16)
} // namespace guest_stat

// Guest PROT_* and MAP_* flags (the subset this increment honours). The values
// match the x86-64 Linux ABI, so a guest passing them gets the kernel's
// interpretation, not a reinterpretation.
namespace mmap_flags {
inline constexpr std::uint64_t kProtNone = 0x0;
inline constexpr std::uint64_t kProtRead = 0x1;
inline constexpr std::uint64_t kProtWrite = 0x2;
inline constexpr std::uint64_t kProtExec = 0x4;
inline constexpr std::uint64_t kMapShared = 0x01;
inline constexpr std::uint64_t kMapPrivate = 0x02;
inline constexpr std::uint64_t kMapAnonymous = 0x20;
inline constexpr std::uint64_t kMapFixed = 0x10;
} // namespace mmap_flags

// lseek(2) whence values.
namespace seek {
inline constexpr std::uint64_t kSet = 0;
inline constexpr std::uint64_t kCur = 1;
inline constexpr std::uint64_t kEnd = 2;
} // namespace seek

// Negative errno values as the guest observes them (returned in rax).
inline constexpr std::int64_t kEperm = 1;
inline constexpr std::int64_t kEnoent = 2;
inline constexpr std::int64_t kEio = 5;
inline constexpr std::int64_t kEbadf = 9;
inline constexpr std::int64_t kEnomem = 12;
inline constexpr std::int64_t kEacces = 13;
inline constexpr std::int64_t kEfault = 14;
inline constexpr std::int64_t kEexist = 17;
inline constexpr std::int64_t kEnotdir = 20;
inline constexpr std::int64_t kEisdir = 21;
inline constexpr std::int64_t kEinval = 22;
inline constexpr std::int64_t kEnfile = 23;
inline constexpr std::int64_t kEmfile = 24;
inline constexpr std::int64_t kEspipe = 29;
inline constexpr std::int64_t kEnosys = 38;
inline constexpr std::int64_t kEoverflow = 75;
inline constexpr std::int64_t kEloop = 40;
inline constexpr std::int64_t kEnametoolong = 36;
inline constexpr std::int64_t kErange = 34;
inline constexpr std::int64_t kEintr = 4;

// What the handler did with a syscall.
enum class SyscallStatus : std::uint8_t {
    kHandled,       // emulated: rax holds the result (or a negative errno)
    kUnimplemented, // not implemented: rax = -ENOSYS, execution continues
    kExit,          // the guest asked to terminate
};

const char* to_string(SyscallStatus status);

struct SyscallResult {
    SyscallStatus status = SyscallStatus::kUnimplemented;
    std::uint64_t number = 0;   // the syscall number requested
    std::int64_t exit_code = 0; // valid when status == kExit
};

// The guest system-call interface. An implementation receives the guest state
// with rax = number and rdi/rsi/rdx/r10/r8/r9 = arguments, and must write the
// result into rax.
class SyscallHandler {
public:
    virtual ~SyscallHandler() = default;
    virtual SyscallResult handle(GuestState& state, Memory& memory) = 0;
};

// Shared POSIX open file description (shared across dup/dup2/dup3/fcntl F_DUPFD).
// Regular files opened via openat(2) store an unbuffered kernel `host_fd` so
// read/write/lseek/pread/pwrite/fstat/getdents64 never suffer C stdio buffer
// desynchronisation; adopted `std::FILE*` streams (such as in-memory test
// streams) keep `stream` and borrow ownership from the caller.
struct OpenFileDescription {
    int host_fd = -1;
    bool owned_fd = false;
    int sink_fd = -1; // 1 or 2 when routed through OutputSink
    std::FILE* stream = nullptr;
    int status_flags = 0;

    ~OpenFileDescription();
    OpenFileDescription() = default;
    OpenFileDescription(const OpenFileDescription&) = delete;
    OpenFileDescription& operator=(const OpenFileDescription&) = delete;
};

// Per-descriptor slot in the guest process's file descriptor table. `cloexec`
// is per-FD (FD_CLOEXEC), whereas file offset and status flags live on `desc`.
struct GuestDescriptor {
    std::shared_ptr<OpenFileDescription> desc;
    bool cloexec = false;

    bool is_open() const noexcept { return desc != nullptr; }
};

// A handler for the minimum needed to run a self-contained guest program that
// prints, reads, allocates memory and terminates. `write` goes to a host sink
// (stdout for fd 1, stderr for fd 2); `read`/`close`/`lseek`/`openat`/`fstat`
// operate on a small guest descriptor table the embedder can seed with adopted
// host descriptors (fd 0 is stdin by default); `mmap`/`mprotect`/`munmap`/`brk`
// operate on the supplied GuestAddressSpace; `exit`/`exit_group` stop the
// program; every other number returns -ENOSYS.
//
// The output sink is injectable so tests can capture guest output without
// touching the test runner's own stdout. The signature mirrors write(2): it
// returns the number of bytes written, or -1 on error.
//
// The address space is supplied by the caller (not owned), so the same
// GuestAddressSpace can be shared with the interpreter or translation cache and
// the handler has no hidden global state. When it is null, the memory syscalls
// return -ENOSYS rather than pretending to succeed.
//
// The descriptor table is owned by the handler (a guest file descriptor is
// handler-local state, like a real process's table). It is not a host file
// descriptor table: the handler maps guest fd -> host FILE* and enforces the
// mapping. `read`/`write` refuse a guest fd that is not in the table with
// -EBADF, so a guest cannot name an arbitrary host descriptor. openat() opens a
// host file on the guest's behalf and allocates a guest fd for it; the stream is
// then handler-owned and closed when the guest closes the fd.
class MinimalSyscallHandler final : public SyscallHandler {
public:
    using OutputSink = std::function<long(int fd, const std::uint8_t* data, std::size_t count)>;

    // With no sink, output is written to the process's stdout/stderr. With no
    // address space, mmap/mprotect/munmap/brk are unimplemented. The descriptor
    // table starts with fd 0/1/2 mapped to the process's stdin/stdout/stderr;
    // see adopt() to add an already-open host stream under a guest fd.
    explicit MinimalSyscallHandler(OutputSink sink = {},
                                   GuestAddressSpace* address_space = nullptr);

    SyscallResult handle(GuestState& state, Memory& memory) override;

    // Map an already-open host stream to a guest descriptor so a guest can read
    // from it. This lets an embedder grant access to a specific host stream
    // without the guest naming a path. Ownership stays with the caller (the
    // FILE* is not fclose'd by the handler), matching the "borrowed, not owned"
    // boundary. Fails (false) if the fd is already taken or out of range.
    bool adopt(int guest_fd, std::FILE* stream);

    // Enable full Box64-style Linux user-space compatibility (`mmap` file-backed
    // shared libraries + `MAP_FIXED`, directory-relative `*at(dirfd, ...)` calls,
    // `ioctl(TCGETS/TIOCGWINSZ)`, `open`/`stat`/`lstat`/`pipe2`/`futex`/signals/
    // `sched_getaffinity`/`sysinfo`/`prctl`, and optional `--sysroot` prefixing).
    void enable_linux_compat(std::string sysroot = {});

private:
    OutputSink sink_;
    GuestAddressSpace* address_space_;
    // Guest fd -> descriptor. Indexed by guest fd, so the guest can never index
    // out of range (the lookup is bounds-checked).
    std::vector<GuestDescriptor> files_;
    std::uint64_t clear_child_tid_ = 0;
    std::uint64_t robust_list_head_ = 0;
    bool linux_compat_ = false;
    std::string sysroot_;
    std::uint64_t sigmask_ = 0;
};

// True if MinimalSyscallHandler implements this syscall number (as opposed to
// returning -ENOSYS). Used by tests and by the compatibility matrix.
bool is_minimal_syscall(std::uint64_t number);

} // namespace hyperion

#endif // HYPERION_SYSCALL_HPP
