// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef CLUSTERING_TABLE_CONTRACT_EXECUTOR_EXECUTOR_HPP_
#define CLUSTERING_TABLE_CONTRACT_EXECUTOR_EXECUTOR_HPP_

#include "clustering/generic/raft_core.hpp"
#include "clustering/immediate_consistency/history.hpp"
#include "clustering/table_contract/contract_metadata.hpp"
#include "clustering/table_contract/cpu_sharding.hpp"
#include "clustering/table_contract/executor/exec.hpp"
#include "concurrency/pump_coro.hpp"
#include "store_subview.hpp"

/* The `contract_executor_t` is responsible for executing the instructions contained in
the `contract_t`s in the `table_raft_state_t`. Each server has one `contract_executor_t`
for each table it's a replica of. The `contract_executor_t` constantly monitors the
current Raft state and compares the contracts in the Raft state against its "executions",
which represent activities it is currently performing. As contracts appear and disappear
from the Raft state, it creates, updates, and destroys executions to match. It also takes
care of forwarding the `contract_ack_t`s generated by the executions back to the
`contract_coordinator_t`. */

class contract_executor_t : public home_thread_mixin_t {
public:
    contract_executor_t(
        const server_id_t &server_id,
        mailbox_manager_t *const mailbox_manager,
        const clone_ptr_t<watchable_t<table_raft_state_t> > &raft_state,
        watchable_map_t<std::pair<server_id_t, branch_id_t>, contract_execution_bcard_t>
            *remote_contract_execution_bcards,
        multistore_ptr_t *multistore,
        const base_path_t &base_path,
        io_backender_t *io_backender,
        backfill_throttler_t *backfill_throttler,
        perfmon_collection_t *perfmons);
    ~contract_executor_t();

    watchable_map_t<std::pair<server_id_t, contract_id_t>, contract_ack_t> *get_acks() {
        return &ack_map;
    }

    watchable_map_t<std::pair<server_id_t, branch_id_t>, contract_execution_bcard_t>
            *get_local_contract_execution_bcards() {
        return &local_contract_execution_bcards;
    }

    watchable_map_t<uuid_u, table_query_bcard_t> *get_local_table_query_bcards() {
        return &local_table_query_bcards;
    }

    range_map_t<key_range_t::right_bound_t, table_shard_status_t> get_shard_status();

private:
    /* The actual work of executing the contract--accepting queries from the user,
    performing backfills, etc.--is carried out by the three `execution_t` subclasses,
    `primary_execution_t`, `secondary_execution_t`, and `erase_execution_t`.
    `execution_data_t` is just a simple wrapper around an `execution_t` with some
    supporting objects. */
    class execution_data_t {
    public:
        /* The contract ID of the contract governing this execution. Note that this may
        change over the course of an execution; see the comment about `execution_key_t`.
        */
        contract_id_t contract_id;

        /* A `store_subview_t` containing only the sub-region of the store that this
        execution affects. */
        scoped_ptr_t<store_subview_t> store_subview;

        /* We create a new perfmon category for each execution. This way the executions
        themselves don't have to think about perfmon key collisions. */
        perfmon_collection_t perfmon_collection;
        scoped_ptr_t<perfmon_membership_t> perfmon_membership;

        /* The execution itself */
        scoped_ptr_t<execution_t> execution;
    };

    /* When a contract changes, we sometimes want to create a new execution and we
    sometimes want to update an existing one. Specifically, we want to create a new
    execution when:
    - The region of the contract changes
    - This server's role in the contract (primary, secondary, or neither) changes
    - This server's role is a secondary but the primary or branch has changed

    We implement this by computing an `execution_key_t` based on each contract
    and the `current_branches` field of the Raft state, using the `get_contract_key()`
    function. If the old and new contracts have the same `execution_key_t`, then we
    update the corresponding execution. But if they differ, then we delete the old
    execution and create a new one. */
    class execution_key_t {
    public:
        enum class role_t { primary, secondary, erase };
        /* This is for generating perfmon keys */
        std::string role_name() const {
            switch (role) {
                case role_t::primary: return "primary";
                case role_t::secondary: return "secondary";
                case role_t::erase: return "erase";
                default: unreachable();
            }
        }
        /* This is just so we can use it as a `std::set`/`std::map` key */
        bool operator<(const execution_key_t &k) const {
            return std::tie(region, role, primary, branch) <
                std::tie(k.region, k.role, k.primary, k.branch);
        }
        region_t region;
        role_t role;
        server_id_t primary;
        branch_id_t branch;
    };

    execution_key_t get_contract_key(
        const std::pair<region_t, contract_t> &pair,
        const branch_id_t &branch);

    /* In response to Raft state changes, we want to delete existing executions and spawn
    new ones. However, deleting executions may block. So `raft_state_subs` notifies
    `update_pumper` which spawns `update_blocking()`. `update_blocking()` calls
    `apply_read()` on the Raft state watchable and passes the result to `update()`.
    `update()` may spawn new executions, but it may not delete them, because that would
    block. Instead, it puts their regions in `to_delete_out`, and then
    `update_blocking()` deletes them. */
    void update_blocking(signal_t *interruptor);
    void update(const table_raft_state_t &new_state,
                std::set<execution_key_t> *to_delete_out);

    /* This will send `cid` and `ack` to the coordinator. We pass it as a callback to
    the `execution_t`. */
    void send_ack(const execution_key_t &key, const contract_id_t &cid,
        const contract_ack_t &ack);

    const server_id_t server_id;
    clone_ptr_t<watchable_t<table_raft_state_t> > raft_state;
    multistore_ptr_t *const multistore;
    perfmon_collection_t *const perfmons;

    /* `ack_map` contains the `contract_ack_t`s created by our execution of contracts.
    It will be sent over the network to the `contract_coordinator_t` via the minidir. */
    watchable_map_var_t<std::pair<server_id_t, contract_id_t>, contract_ack_t> ack_map;

    /* `local_contract_execution_bcards` contains the `contract_execution_bcards` for
    our `primary_execution_t`s. It will be sent over the network to the other
    `contract_executor_t`s for this table, via the minidir, so that they can request
    backfills from us and connect their `listener_t`s to our `broadcaster_t`s. */
    watchable_map_var_t<std::pair<server_id_t, branch_id_t>, contract_execution_bcard_t>
        local_contract_execution_bcards;

    /* `local_table_query_bcards` contains the `table_query_bcard_t`s for our
    `primary_execution_t`s. It will be sent over the network to all the servers in the
    cluster, via the directory, so that they can run queries. */
    watchable_map_var_t<uuid_u, table_query_bcard_t> local_table_query_bcards;

    /* This is just a convenient struct to hold a bunch of objects that the
    `execution_t`s need access to. */
    execution_t::context_t execution_context;

    std::map<execution_key_t, scoped_ptr_t<execution_data_t> > executions;

    /* Used to generate unique names for perfmons */
    int perfmon_counter;

    /* `update_pumper` calls `update_blocking()`. Destructor order matters: we must
    destroy `raft_state_subs` before `update_pumper` because it notifies `update_pumper`,
    but we must destroy `update_pumper` before the other member variables because
    `update_blocking()` accesses them. */
    scoped_ptr_t<pump_coro_t> update_pumper;

    /* We subscribe to changes in the Raft committed state so we can find out when a new
    contract has been issued. */
    watchable_t<table_raft_state_t>::subscription_t raft_state_subs;
};

#endif /* CLUSTERING_TABLE_CONTRACT_EXECUTOR_EXECUTOR_HPP_ */

