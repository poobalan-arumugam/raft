#include "../../include/raft.h"

#include "../lib/heap.h"
#include "../lib/logger.h"
#include "../lib/munit.h"

/**
 * Helpers
 */

struct fixture
{
    struct raft_heap heap;
    struct raft_logger logger;
    struct raft_io io;
    struct
    {
        bool invoked;
        unsigned elapsed; /* Milliseconds since last call to __tick */
    } tick_cb;
    struct
    {
        bool invoked;
        int status;
    } append_cb;
    struct
    {
        bool invoked;
        int status;
    } send_cb;
    struct
    {
        bool invoked;
        struct raft_message *message;
    } recv_cb;
    struct
    {
        bool invoked;
    } stop_cb;
};

static void __tick_cb(void *data, const unsigned elapsed)
{
    struct fixture *f = data;

    f->tick_cb.invoked = true;
    f->tick_cb.elapsed = elapsed;
}

static void __append_cb(void *data, const int status)
{
    struct fixture *f = data;

    f->append_cb.invoked = true;
    f->append_cb.status = status;
}

static void __send_cb(void *data, const int status)
{
    struct fixture *f = data;

    f->send_cb.invoked = true;
    f->send_cb.status = status;
}

static void __recv_cb(void *data, struct raft_message *message)
{
    struct fixture *f = data;

    f->recv_cb.invoked = true;
    f->recv_cb.message = message;
}

static void __stop_cb(void *data)
{
    struct fixture *f = data;

    f->stop_cb.invoked = true;
}

static void *setup(const MunitParameter params[], void *user_data)
{
    struct fixture *f = munit_malloc(sizeof *f);
    const uint64_t id = 1;
    int rv;

    (void)user_data;

    test_heap_setup(params, &f->heap);
    test_logger_setup(params, &f->logger, id);

    rv = raft_io_stub_init(&f->io, &f->logger);
    munit_assert_int(rv, ==, 0);

    rv = f->io.start(&f->io, 1, "1", 50, f, __tick_cb, __recv_cb);
    munit_assert_int(rv, ==, 0);

    f->tick_cb.invoked = false;
    f->tick_cb.elapsed = 0;

    f->append_cb.invoked = false;
    f->append_cb.status = -1;

    f->send_cb.invoked = false;
    f->send_cb.status = -1;

    f->stop_cb.invoked = false;

    return f;
}

static void tear_down(void *data)
{
    struct fixture *f = data;

    f->io.stop(&f->io, f, __stop_cb);

    munit_assert_true(f->stop_cb.invoked);

    raft_io_stub_close(&f->io);

    test_logger_tear_down(&f->logger);
    test_heap_tear_down(&f->heap);

    free(f);
}

/**
 * Advance time.
 */
#define __advance(F, MSECS)                  \
    {                                        \
        raft_io_stub_advance(&F->io, MSECS); \
    }

/**
 * Load the initial state from the store and check that no error occurs.
 */
#define __load(F)                                                          \
    {                                                                      \
        raft_term term;                                                    \
        unsigned voted_for;                                                \
        raft_index start_index;                                            \
        struct raft_entry *entries;                                        \
        size_t n_entries;                                                  \
        int rv;                                                            \
                                                                           \
        rv = F->io.load(&F->io, &term, &voted_for, &start_index, &entries, \
                        &n_entries);                                       \
        munit_assert_int(rv, ==, 0);                                       \
    }

/**
 * raft_io_stub__start
 */

/* When raft_io_stub_advance is called, the tick callback is invoked. */
static MunitResult test_start_tick(const MunitParameter params[], void *data)
{
    struct fixture *f = data;

    (void)params;

    __advance(f, 100);

    munit_assert_true(f->tick_cb.invoked);
    munit_assert_int(f->tick_cb.elapsed, ==, 100);

    return MUNIT_OK;
}

/* Once the raft_io_uv instance is started, the recv callback is invoked when a
 * message is received.. */
static MunitResult test_start_recv(const MunitParameter params[], void *data)
{
    struct fixture *f = data;
    struct raft_message message;

    (void)params;

    message.type = RAFT_IO_REQUEST_VOTE;
    message.server_id = 2;
    message.server_address = "2";

    raft_io_stub_dispatch(&f->io, &message);

    munit_assert_true(f->recv_cb.invoked);
    munit_assert_int(f->recv_cb.message->type, ==, RAFT_IO_REQUEST_VOTE);
    munit_assert_int(f->recv_cb.message->server_id, ==, 2);
    munit_assert_string_equal(f->recv_cb.message->server_address, "2");

    return MUNIT_OK;
}

static MunitTest start_tests[] = {
    {"/tick", test_start_tick, setup, tear_down, 0, NULL},
    {"/recv", test_start_recv, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

/**
 * raft_io_stub__load
 */

/**
 * Load the initial state of a pristine server.
 */
static MunitResult test_load_pristine(const MunitParameter params[], void *data)
{
    struct fixture *f = data;
    raft_term term;
    unsigned voted_for;
    raft_index start_index;
    struct raft_entry *entries;
    size_t n_entries;
    int rv;

    (void)params;

    rv = f->io.load(&f->io, &term, &voted_for, &start_index, &entries,
                    &n_entries);
    munit_assert_int(rv, ==, 0);

    munit_assert_int(term, ==, 0);
    munit_assert_int(voted_for, ==, 0);
    munit_assert_int(start_index, ==, 1);
    munit_assert_ptr_null(entries);
    munit_assert_int(n_entries, ==, 0);

    return MUNIT_OK;
}

static MunitTest load_tests[] = {
    {"/pristine", test_load_pristine, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

/**
 * raft_io_stub__bootstrap
 */

/**
 * Bootstrap a pristine server.
 */
static MunitResult test_bootstrap_pristine(const MunitParameter params[],
                                           void *data)
{
    struct fixture *f = data;
    struct raft_configuration configuration;
    int rv;

    (void)params;

    __load(f);

    /* Create a configuration */
    raft_configuration_init(&configuration);

    rv = raft_configuration_add(&configuration, 1, "1", true);
    munit_assert_int(rv, ==, 0);

    rv = f->io.bootstrap(&f->io, &configuration);
    munit_assert_int(rv, ==, 0);

    raft_configuration_close(&configuration);

    return MUNIT_OK;
}

static MunitTest bootstrap_tests[] = {
    {"/pristine", test_bootstrap_pristine, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

/**
 * raft_io_stub__set_term
 */

/**
 * Set the term on a pristine store.
 */
static MunitResult test_set_term_pristine(const MunitParameter params[],
                                          void *data)
{
    struct fixture *f = data;
    int rv;

    (void)params;

    __load(f);

    rv = f->io.set_term(&f->io, 1);
    munit_assert_int(rv, ==, 0);

    return MUNIT_OK;
}

static MunitTest set_term_tests[] = {
    {"/pristine", test_set_term_pristine, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

/**
 * raft_io_stub__set_vote
 */

/**
 * Set the vote on a pristine store.
 */

static MunitResult test_set_vote_pristine(const MunitParameter params[],
                                          void *data)
{
    struct fixture *f = data;
    int rv;

    (void)params;

    __load(f);

    rv = f->io.set_term(&f->io, 1);
    munit_assert_int(rv, ==, 0);

    rv = f->io.set_vote(&f->io, 2);
    munit_assert_int(rv, ==, 0);

    return MUNIT_OK;
}

static MunitTest set_vote_tests[] = {
    {"/pristine", test_set_vote_pristine, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

/**
 * raft_io_stub__append
 */

/**
 * Append entries on a pristine store.
 */

static MunitResult test_append_pristine(const MunitParameter params[],
                                        void *data)
{
    struct fixture *f = data;
    struct raft_entry entry;
    int rv;

    (void)params;

    __load(f);

    entry.term = 1;
    entry.type = RAFT_LOG_COMMAND;
    entry.buf.base = munit_malloc(1);
    entry.buf.len = 1;

    ((char *)entry.buf.base)[0] = 'x';

    rv = f->io.append(&f->io, &entry, 1, f, __append_cb);
    munit_assert_int(rv, ==, 0);

    raft_io_stub_flush(&f->io);

    munit_assert_true(f->append_cb.invoked);

    free(entry.buf.base);

    return MUNIT_OK;
}

/**
 * raft_io_stub__send
 */

/**
 * Send the very first message.
 */

static MunitResult test_send_first(const MunitParameter params[], void *data)
{
    struct fixture *f = data;
    struct raft_message message;
    int rv;

    (void)params;

    __load(f);

    message.type = RAFT_IO_REQUEST_VOTE;
    message.server_id = 2;
    message.server_address = "2";

    rv = f->io.send(&f->io, &message, f, __send_cb);
    munit_assert_int(rv, ==, 0);

    raft_io_stub_flush(&f->io);

    munit_assert_true(f->send_cb.invoked);

    return MUNIT_OK;
}

static MunitTest send_tests[] = {
    {"/first", test_send_first, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

static MunitTest append_tests[] = {
    {"/pristine", test_append_pristine, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

/**
 * Test suite
 */

MunitSuite raft_io_stub_suites[] = {
    {"/start", start_tests, NULL, 1, 0},
    {"/load", load_tests, NULL, 1, 0},
    {"/bootstrap", bootstrap_tests, NULL, 1, 0},
    {"/set-term", set_term_tests, NULL, 1, 0},
    {"/set-vote", set_vote_tests, NULL, 1, 0},
    {"/append", append_tests, NULL, 1, 0},
    {"/send", send_tests, NULL, 1, 0},
    {NULL, NULL, NULL, 0, 0},
};
