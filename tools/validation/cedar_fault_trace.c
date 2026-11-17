/* SPDX-License-Identifier: MIT
 * Test-service-only fatal SIGSEGV recorder. No recovery or vendor interposition.
 * Handler uses lock-free atomics and async-signal-safe POSIX calls only.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "fatal guard must be lock-free");
static _Atomic int entered;
static int memory_fd = -1, maps_fd = -1;
static unsigned char alternate_stack[65536] __attribute__((aligned(64)));

struct line { char data[384]; size_t size; };

static void emit(const char *data, size_t size)
{
    while (size) {
        ssize_t count = write(STDERR_FILENO, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        data += count; size -= (size_t)count;
    }
}

static void literal(struct line *line, const char *text)
{
    while (*text && line->size < sizeof(line->data)) line->data[line->size++] = *text++;
}

static void hex(struct line *line, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    literal(line, "0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        if (line->size < sizeof(line->data)) line->data[line->size++] = digits[(value >> shift) & 15];
}

static void field(struct line *line, const char *name, uint64_t value)
{ literal(line, name); hex(line, value); }

static void finish(struct line *line)
{ literal(line, "\n"); emit(line->data, line->size); }

static void fatal_fault(int signal_number, siginfo_t *info, void *opaque)
{
    if (atomic_exchange_explicit(&entered, 1, memory_order_relaxed))
        _exit(128 + signal_number);
    const ucontext_t *context = opaque;
    uint64_t pc = 0, lr = 0, fp = 0, sp = 0, state = 0;
#if defined(__aarch64__)
    pc = context->uc_mcontext.pc; lr = context->uc_mcontext.regs[30];
    fp = context->uc_mcontext.regs[29]; sp = context->uc_mcontext.sp;
    state = context->uc_mcontext.pstate;
#elif defined(__x86_64__)
    pc = (uint64_t)context->uc_mcontext.gregs[REG_RIP];
    fp = (uint64_t)context->uc_mcontext.gregs[REG_RBP];
    sp = (uint64_t)context->uc_mcontext.gregs[REG_RSP];
    state = (uint64_t)context->uc_mcontext.gregs[REG_EFL];
#else
#error Unsupported diagnostic architecture
#endif
    struct line line;
    line.size = 0;
    literal(&line, "T736_FAULT sig="); hex(&line, (uint64_t)signal_number);
    field(&line, " code=", (uint64_t)(int64_t)(info ? info->si_code : 0));
    field(&line, " address=", info ? (uint64_t)(uintptr_t)info->si_addr : 0);
    field(&line, " pc=", pc); field(&line, " lr=", lr);
    field(&line, " fp=", fp); field(&line, " sp=", sp);
    field(&line, " pstate=", state); finish(&line);
#if defined(__aarch64__)
    /* Capturing all general registers often identifies a null call target or
     * invalid plugin argument even if vendor code omits frame pointers. */
    for (unsigned first = 0; first < 29; first += 4) {
        line.size = 0; literal(&line, "T736_REGS first="); hex(&line, first);
        for (unsigned i = first; i < first + 4 && i < 29; i++)
            field(&line, " x=", context->uc_mcontext.regs[i]);
        finish(&line);
    }
#endif
    /* Never dereference a possibly corrupt stack/frame pointer in userspace.
     * /proc/self/mem pread is bounded and returns EIO for unreadable addresses.
     * Normal AArch64 and x86_64 frame records are previousFP,returnAddress. */
    for (unsigned depth = 0; memory_fd >= 0 && depth < 16; depth++) {
        uint64_t words[2];
#if defined(__aarch64__)
        const uint64_t alignment_mask = 15;
#else
        const uint64_t alignment_mask = 7;
#endif
        if (!fp || (fp & alignment_mask) || fp < sp || fp - sp > UINT64_C(8) * 1024 * 1024 ||
            fp > INT64_MAX - sizeof(words)) break;
        ssize_t count = pread(memory_fd, words, sizeof(words), (off_t)fp);
        if (count != (ssize_t)sizeof(words)) break;
        line.size = 0; literal(&line, "T736_FP depth="); hex(&line, depth);
        field(&line, " fp=", fp); field(&line, " next=", words[0]);
        field(&line, " return=", words[1]); finish(&line);
        if (words[0] <= fp || words[0] - fp > 1024 * 1024) break;
        fp = words[0];
    }
    /* The descriptor was opened before the failure; reading now includes
     * libraries that the vendor/plugin loader dlopened after construction. */
    if (maps_fd >= 0 && lseek(maps_fd, 0, SEEK_SET) >= 0) {
        static const char begin[] = "T736_MAPS_BEGIN\n", end[] = "T736_MAPS_END\n";
        char buffer[2048]; size_t remaining = 65536;
        emit(begin, sizeof(begin) - 1);
        while (remaining) {
            size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            ssize_t count = read(maps_fd, buffer, wanted);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            emit(buffer, (size_t)count); remaining -= (size_t)count;
        }
        emit(end, sizeof(end) - 1);
    }
    static const char done[] = "T736_FAULT_EXIT no-recovery no-retry\n";
    emit(done, sizeof(done) - 1);
    _exit(128 + signal_number);
}

__attribute__((constructor)) static void install_fault_trace(void)
{
    memory_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    maps_fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    /* Alternate stacks are per-thread. This protects the constructor thread;
     * worker-thread faults use their ordinary stack without pthread hooks. */
    stack_t alternate = { .ss_sp = alternate_stack, .ss_size = sizeof(alternate_stack) };
    int alternate_result = sigaltstack(&alternate, NULL);
    struct sigaction action = { .sa_sigaction = fatal_fault,
        .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL)) {
        static const char error[] = "T736_FAULT_SETUP_FAILED sigaction\n";
        emit(error, sizeof(error) - 1); _exit(127);
    }
    struct line line = { .size = 0 };
    literal(&line, "T736_FAULT_READY pid="); hex(&line, (uint64_t)getpid());
    field(&line, " mem_fd=", (uint64_t)(int64_t)memory_fd);
    field(&line, " maps_fd=", (uint64_t)(int64_t)maps_fd);
    field(&line, " main_altstack_rc=", (uint64_t)(int64_t)alternate_result);
    literal(&line, " max_frames=16 max_maps_bytes=65536"); finish(&line);
}
