// Hand-rolled tests for the core module. No test framework, no new macros: every
// check is an ASSERT from core/assert.h, so a failure aborts with a located
// message and a non-zero exit, which is all CI needs.

#include "core/arena.h"
#include "core/array.h"
#include "core/assert.h"
#include "core/log.h"
#include "core/pool.h"
#include "core/result.h"
#include "core/span.h"
#include "core/timer.h"
#include "core/types.h"

using namespace core;

static void test_arena_alignment_and_rewind() {
    Arena a = Arena::with_capacity(4096);
    ASSERT(a.used() == 0);

    void* p1 = a.alloc(8, 8);
    ASSERT((reinterpret_cast<u64>(p1) % 8) == 0);
    ASSERT(a.used() >= 8);

    const u64 mark = a.marker();
    void* p2 = a.alloc(16, 16);
    ASSERT((reinterpret_cast<u64>(p2) % 16) == 0);

    a.rewind(mark);
    ASSERT(a.used() == mark);

    a.reset();
    ASSERT(a.used() == 0);
}

static void test_permanent_arena_stable_over_frames() {
    // A per-frame arena reset each frame must not disturb a permanent arena that
    // the loop never touches. This is the shape of M0 acceptance check 4.
    Arena permanent = Arena::with_capacity(1u << 20);
    Arena frame = Arena::with_capacity(1u << 20);

    (void)permanent.alloc(128, 16);
    const u64 permanent_off = permanent.used();

    for (i32 i = 0; i < 1000; ++i) {
        frame.reset();
        (void)frame.alloc(4096, 16);
    }

    ASSERT(permanent.used() == permanent_off);
}

static void test_array() {
    Arena a = Arena::with_capacity(4096);
    Array<u32> xs = a.make_array<u32>(4);
    ASSERT(xs.empty());

    xs.push(10);
    xs.push(20);
    xs.push(30);
    xs.push(40);
    ASSERT(xs.full());
    ASSERT(!xs.try_push(50));
    ASSERT(xs.size() == 4);
    ASSERT(xs[2] == 30);

    u32 sum = 0;
    for (const u32 v : xs) {
        sum += v;
    }
    ASSERT(sum == 100);

    const Span<u32> s = xs.as_span();
    ASSERT(s.size() == 4);
    ASSERT(s[0] == 10);

    xs.clear();
    ASSERT(xs.empty());
    ASSERT(xs.capacity() == 4);
}

enum class ParseError : u8 { TooLong };

static Result<u32, ParseError> parse(bool ok) {
    if (ok) {
        return Result<u32, ParseError>::ok(42);
    }
    return Result<u32, ParseError>::err(ParseError::TooLong);
}

static void test_result() {
    Result<u32, ParseError> good = parse(true);
    ASSERT(good.is_ok());
    ASSERT(good.value() == 42);

    Result<u32, ParseError> bad = parse(false);
    ASSERT(bad.is_err());
    ASSERT(bad.error() == ParseError::TooLong);

    // Move assignment must switch the live member cleanly.
    good = parse(false);
    ASSERT(good.is_err());
}

static void test_timer_monotonic() {
    const u64 t0 = Timer::now_ns();
    const u64 t1 = Timer::now_ns();
    ASSERT(t1 >= t0);
}

static void test_pool_insert_get_remove() {
    Arena a = Arena::with_capacity(1u << 16);
    Pool<u32> pool = Pool<u32>::with_capacity(a, 4);
    ASSERT(pool.size() == 0);
    ASSERT(pool.capacity() == 4);

    const Handle<u32> h1 = pool.insert(11);
    const Handle<u32> h2 = pool.insert(22);
    ASSERT(pool.size() == 2);
    ASSERT(*pool.get(h1) == 11);
    ASSERT(*pool.get(h2) == 22);

    pool.remove(h1);
    ASSERT(pool.size() == 1);
    ASSERT(pool.get(h1) == nullptr);  // stale handle resolves to null
    ASSERT(*pool.get(h2) == 22);      // unrelated handle unaffected
}

static void test_pool_stale_after_reuse() {
    Arena a = Arena::with_capacity(1u << 16);
    Pool<u32> pool = Pool<u32>::with_capacity(a, 2);

    const Handle<u32> old = pool.insert(1);
    pool.remove(old);

    // The freed slot is reused, but the old handle must not see the newcomer.
    const Handle<u32> fresh = pool.insert(2);
    ASSERT(fresh.index == old.index);
    ASSERT(fresh.gen != old.gen);
    ASSERT(pool.get(old) == nullptr);
    ASSERT(*pool.get(fresh) == 2);
}

static void test_pool_fill_and_drain() {
    Arena a = Arena::with_capacity(1u << 16);
    Pool<u32> pool = Pool<u32>::with_capacity(a, 8);

    Handle<u32> handles[8];
    for (u32 i = 0; i < 8; ++i) {
        handles[i] = pool.insert(i * 10);
    }
    ASSERT(pool.size() == 8);
    for (u32 i = 0; i < 8; ++i) {
        ASSERT(*pool.get(handles[i]) == i * 10);
        pool.remove(handles[i]);
    }
    ASSERT(pool.size() == 0);

    // Fully drained pool accepts a full refill.
    for (u32 i = 0; i < 8; ++i) {
        (void)pool.insert(i);
    }
    ASSERT(pool.size() == 8);

    ASSERT(pool.get(Handle<u32>::none()) == nullptr);
    ASSERT(!Handle<u32>::none().is_some());
}

int main() {
    test_arena_alignment_and_rewind();
    test_permanent_arena_stable_over_frames();
    test_array();
    test_result();
    test_timer_monotonic();
    test_pool_insert_get_remove();
    test_pool_stale_after_reuse();
    test_pool_fill_and_drain();
    log_info("core_tests: all passed");
    return 0;
}
