// mini_uring.h (Corrected Version)
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>

// Required system headers for raw io_uring access
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <linux/io_uring.h>

// Forward declaration
struct io_uring;

// --- Syscall Wrappers ---
static inline int io_uring_setup(unsigned int entries, struct io_uring_params *p) {
    return (int) syscall(__NR_io_uring_setup, entries, p);
}

static inline int io_uring_enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags, sigset_t *sig) {
    return (int) syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, sig, sizeof(sigset_t));
}

static inline int io_uring_register(int ring_fd, unsigned int opcode, void *arg, unsigned int nr_args) {
    return (int) syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}


// --- Re-implementation of liburing structs and functions ---
struct io_uring {
    struct io_uring_sq {
        unsigned *head;
        unsigned *tail;
        unsigned *ring_mask;
        unsigned *ring_entries;
        unsigned *flags;
        unsigned *array;
    } sq;

    struct io_uring_cq {
        unsigned *head;
        unsigned *tail;
        unsigned *ring_mask;
        unsigned *ring_entries;
        struct io_uring_cqe *cqes;
    } cq;

    struct io_uring_sqe *sqes;
    int ring_fd;
    unsigned int sq_tail_cached;
};

// --- Queue Management ---
static inline int io_uring_queue_init(unsigned int entries, struct io_uring *ring, unsigned int flags) {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = flags;

    int fd = io_uring_setup(entries, &p);
    if (fd < 0) return fd;

    size_t sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

    void *sq_ptr = mmap(0, sq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) { close(fd); return -errno; }

    void *cq_ptr = mmap(0, cq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) { munmap(sq_ptr, sq_ring_sz); close(fd); return -errno; }

    ring->sqes = (io_uring_sqe*)mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (ring->sqes == MAP_FAILED) { munmap(cq_ptr, cq_ring_sz); munmap(sq_ptr, sq_ring_sz); close(fd); return -errno; }

    ring->sq.head = (unsigned*)((char*)sq_ptr + p.sq_off.head);
    ring->sq.tail = (unsigned*)((char*)sq_ptr + p.sq_off.tail);
    ring->sq.ring_mask = (unsigned*)((char*)sq_ptr + p.sq_off.ring_mask);
    ring->sq.ring_entries = (unsigned*)((char*)sq_ptr + p.sq_off.ring_entries);
    ring->sq.flags = (unsigned*)((char*)sq_ptr + p.sq_off.flags);
    ring->sq.array = (unsigned*)((char*)sq_ptr + p.sq_off.array);

    ring->cq.head = (unsigned*)((char*)cq_ptr + p.cq_off.head);
    ring->cq.tail = (unsigned*)((char*)cq_ptr + p.cq_off.tail);
    ring->cq.ring_mask = (unsigned*)((char*)cq_ptr + p.cq_off.ring_mask);
    ring->cq.ring_entries = (unsigned*)((char*)cq_ptr + p.cq_off.ring_entries);
    ring->cq.cqes = (io_uring_cqe*)((char*)cq_ptr + p.cq_off.cqes);

    ring->ring_fd = fd;
    ring->sq_tail_cached = *ring->sq.tail;
    return 0;
}

static inline void io_uring_queue_exit(struct io_uring *ring) {
    if (ring->ring_fd == -1) return;
    // Simplified unmap logic
    munmap(ring->sqes, *ring->sq.ring_entries * sizeof(struct io_uring_sqe));
    munmap(ring->sq.head, *ring->sq.ring_entries * sizeof(unsigned) + sizeof(struct io_uring_sqe));
    munmap(ring->cq.head, *ring->cq.ring_entries * sizeof(struct io_uring_cqe) * 2);
    close(ring->ring_fd);
    ring->ring_fd = -1;
}

// --- Submission Queue Management ---
static inline struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring) {
    unsigned head = std::atomic_load_explicit((std::atomic<unsigned>*)ring->sq.head, std::memory_order_acquire);
    if (ring->sq_tail_cached - head < *ring->sq.ring_entries) {
        struct io_uring_sqe *sqe = &ring->sqes[ring->sq_tail_cached & *ring->sq.ring_mask];
        ring->sq_tail_cached++;
        return sqe;
    }
    return nullptr;
}

// ##################################################################
// #                        CORRECTED FUNCTION                        #
// ##################################################################
static inline int io_uring_submit(struct io_uring *ring) {
    unsigned tail = *ring->sq.tail;
    unsigned submitted = ring->sq_tail_cached - tail;

    if (submitted == 0) {
        return 0;
    }

    unsigned mask = *ring->sq.ring_mask;

    // *** THE FIX IS HERE ***
    // We must populate the indirection array (sq.array) with the indices
    // of the SQEs we have prepared. The kernel reads this array.
    for (unsigned i = 0; i < submitted; i++) {
        unsigned index = (tail + i) & mask;
        ring->sq.array[index] = index;
    }

    // Now, make the new tail visible to the kernel. This must be done
    // with a memory barrier to ensure the sq.array writes are visible first.
    std::atomic_store_explicit((std::atomic<unsigned>*)ring->sq.tail, ring->sq_tail_cached, std::memory_order_release);

    // Ask the kernel to process the 'submitted' number of new entries.
    int ret = io_uring_enter(ring->ring_fd, submitted, 0, 0, nullptr);
    return ret;
}

// --- SQE Preparation Helpers ---
static inline void io_uring_prep_read(struct io_uring_sqe *sqe, int fd, void *buf, unsigned nbytes, off_t offset) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = (unsigned long)buf;
    sqe->len = nbytes;
}

static inline void io_uring_prep_write(struct io_uring_sqe *sqe, int fd, const void *buf, unsigned nbytes, off_t offset) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = (unsigned long)buf;
    sqe->len = nbytes;
}

static inline void io_uring_sqe_set_data(struct io_uring_sqe *sqe, void *data) {
    sqe->user_data = (uint64_t)data;
}

// --- Completion Queue Management ---
static inline void io_uring_cq_advance(struct io_uring *ring, unsigned count) {
    if (count > 0) {
        std::atomic_store_explicit((std::atomic<unsigned>*)ring->cq.head, *ring->cq.head + count, std::memory_order_release);
    }
}

#define io_uring_for_each_cqe(ring, head, cqe) \
    for (head = *(ring)->cq.head; \
         head != std::atomic_load_explicit((std::atomic<unsigned>*)(ring)->cq.tail, std::memory_order_acquire) && (cqe = &(ring)->cq.cqes[head & *(ring)->cq.ring_mask], 1); \
         head++)

// --- Registration ---
static inline int io_uring_register_files(struct io_uring *ring, const int *files, unsigned nr_files) {
    return io_uring_register(ring->ring_fd, IORING_REGISTER_FILES, (void*)files, nr_files);
}

static inline int io_uring_unregister_files(struct io_uring *ring) {
    return io_uring_register(ring->ring_fd, IORING_UNREGISTER_FILES, nullptr, 0);
}

// --- Other helpers from your code ---
static inline unsigned io_uring_sq_ready(struct io_uring *ring) {
    return ring->sq_tail_cached - *ring->sq.tail;
}