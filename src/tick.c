#include "../include/raft.h"

#include "assert.h"
#include "configuration.h"
#include "election.h"
#include "replication.h"
#include "state.h"
#include "watch.h"

/**
 * Number of milliseconds after which a server promotion will be aborted if the
 * server hasn't caught up with the logs yet.
 */
#define RAFT_MAX_CATCH_UP_DURATION (30 * 1000)

/**
 * Apply time-dependent rules for followers (Figure 3.1).
 */
static int raft_tick__follower(struct raft *r)
{
    const struct raft_server *server;
    int rv;

    assert(r != NULL);
    assert(r->state == RAFT_STATE_FOLLOWER);

    server = raft_configuration__get(&r->configuration, r->id);

    /* If we have been removed from the configuration, or maybe we didn't
     * receive one yet, just stay follower. */
    if (server == NULL) {
        return 0;
    }

    /* If there's only one voting server, and that is us, it's safe to convert
     * to leader. If that is not us, we're either joining the cluster or we're
     * simply configured as non-voter, do nothing and wait for RPCs. */
    if (raft_configuration__n_voting(&r->configuration) == 1) {
        if (server->voting) {
            raft_debugf(r->logger, "tick: self elect and convert to leader");
            rv = raft_state__convert_to_candidate(r);
            if (rv != 0) {
                return rv;
            }
            rv = raft_state__convert_to_leader(r);
            if (rv != 0) {
                return rv;
            }
            return 0;
        }

        return 0;
    }

    /* Check if we need to start an election.
     *
     * From Section §3.3:
     *
     *   If a follower receives no communication over a period of time called
     *   the election timeout, then it assumes there is no viable leader and
     *   begins an election to choose a new leader.
     *
     * Figure 3.1:
     *
     *   If election timeout elapses without receiving AppendEntries RPC from
     *   current leader or granting vote to candidate, convert to candidate.
     */
    if (r->timer > r->election_timeout_rand && server->voting) {
        raft_infof(r->logger,
                   "tick: convert to candidate and start new election");
        return raft_state__convert_to_candidate(r);
    }

    return 0;
}

/**
 * Apply time-dependent rules for candidates (Figure 3.1).
 */
static int raft_tick__candidate(struct raft *r)
{
    assert(r != NULL);
    assert(r->state == RAFT_STATE_CANDIDATE);

    /* Check if we need to start an election.
     *
     * From Section §3.4:
     *
     *   The third possible outcome is that a candidate neither wins nor loses
     *   the election: if many followers become candidates at the same time,
     *   votes could be split so that no candidate obtains a majority. When this
     *   happens, each candidate will time out and start a new election by
     *   incrementing its term and initiating another round of RequestVote RPCs
     */
    if (r->timer > r->election_timeout_rand) {
        raft_infof(r->logger, "tick: start new election");
        return raft_election__start(r);
    }

    return 0;
}

/**
 * Apply time-dependent rules for leaders (Figure 3.1).
 */
static int raft_tick__leader(struct raft *r,
                             const unsigned msec_since_last_tick)
{
    assert(r != NULL);
    assert(r->state == RAFT_STATE_LEADER);

    /* Check if we need to send heartbeats.
     *
     * From Figure 3.1:
     *
     *   Send empty AppendEntries RPC during idle periods to prevent election
     *   timeouts.
     */
    if (r->timer > r->heartbeat_timeout) {
        raft_replication__trigger(r, 0);
        r->timer = 0;
    }

    /* If a server is being promoted, increment the timer of the current
     * round or abort the promotion.
     *
     * From Section 4.2.1:
     *
     *   The algorithm waits a fixed number of rounds (such as 10). If the last
     *   round lasts less than an election timeout, then the leader adds the new
     *   server to the cluster, under the assumption that there are not enough
     *   unreplicated entries to create a significant availability
     *   gap. Otherwise, the leader aborts the configuration change with an
     *   error.
     */
    if (r->leader_state.promotee_id != 0) {
        unsigned id = r->leader_state.promotee_id;
        size_t server_index;
        bool is_too_slow;
        bool is_unresponsive;

        /* If a promotion is in progress, we expect that our configuration
         * contains an entry for the server being promoted, and that the server
         * is not yet considered as voting. */
        server_index = raft_configuration__index(&r->configuration, id);
        assert(server_index < r->configuration.n);
        assert(!r->configuration.servers[server_index].voting);

        r->leader_state.round_duration += msec_since_last_tick;

        is_too_slow = (r->leader_state.round_number == 10 &&
                       r->leader_state.round_duration > r->election_timeout);
        is_unresponsive =
            r->leader_state.round_duration > RAFT_MAX_CATCH_UP_DURATION;

        /* Abort the promotion if we are at the 10'th round and it's still
         * taking too long, or if the server is unresponsive. */
        if (is_too_slow || is_unresponsive) {
            r->leader_state.promotee_id = 0;

            r->leader_state.round_index = 0;
            r->leader_state.round_number = 0;
            r->leader_state.round_duration = 0;

            raft_watch__promotion_aborted(r, id);
        }
    }

    return 0;
}

int raft__tick(struct raft *r, const unsigned msec_since_last_tick)
{
    int rv;

    assert(r != NULL);

    assert(r->state == RAFT_STATE_UNAVAILABLE ||
           r->state == RAFT_STATE_FOLLOWER ||
           r->state == RAFT_STATE_CANDIDATE || r->state == RAFT_STATE_LEADER);

    /* If we are not available, let's do nothing. */
    if (r->state == RAFT_STATE_UNAVAILABLE) {
        return 0;
    }

    r->timer += msec_since_last_tick;

    switch (r->state) {
        case RAFT_STATE_FOLLOWER:
            rv = raft_tick__follower(r);
            break;
        case RAFT_STATE_CANDIDATE:
            rv = raft_tick__candidate(r);
            break;
        case RAFT_STATE_LEADER:
            rv = raft_tick__leader(r, msec_since_last_tick);
            break;
        default:
            rv = RAFT_ERR_INTERNAL;
            break;
    }

    return rv;
}
