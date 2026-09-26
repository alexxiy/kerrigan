#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
feature_llmq_dkg_nonmember_relay.py

A DKG message whose sender is not a member of the quorum, as this node computes
it, must be dropped without punishing the peer that delivered it.

DKG messages are relayed. The peer that hands us a contribution, complaint,
justification or premature commitment is usually not its author, and it only
relays what its own view of the quorum accepted. If two nodes disagree about
who is a member, scoring the delivering peer 100 discourages an honest relayer.
On mainnet this happened every DKG round: masternodes discouraged each other,
the quorum mesh collapsed, and ChainLocks and InstantSend stalled until the
discourage filters were cleared by hand.

The test crafts a contribution for the live session with a proTxHash that is
not a member, sends it from a test peer during the contribution phase, and
waits for the node to log that it skipped the message, naming the rejected
proTxHash so operators can see which member their view disputes. It then
asserts no ban was issued. A malformed contribution sent afterwards must still be punished,
which proves the harness can observe a ban at all.
"""

import os

from test_framework.messages import ser_compact_size, ser_uint256
from test_framework.p2p import P2PInterface
from test_framework.test_framework import KerriganTestFramework

LLMQ_TEST = 100
BLS_PUBKEY_SIZE = 48
BLS_SIG_SIZE = 96


class msg_qcontrib_raw:
    __slots__ = ("data",)
    msgtype = b"qcontrib"

    def __init__(self, data):
        self.data = data

    def serialize(self):
        return self.data


def nonmember_contribution(quorum_hash, pro_tx_hash):
    """A contribution that deserializes cleanly (all-zero BLS objects are null,
    not malformed) and fails preverification only on membership."""
    data = bytes([LLMQ_TEST])
    data += ser_uint256(int(quorum_hash, 16))
    data += ser_uint256(int(pro_tx_hash, 16))
    data += ser_compact_size(0)               # vvec
    data += bytes(BLS_PUBKEY_SIZE)            # contributions.ephemeralPubKey
    data += bytes(32)                         # contributions.ivSeed
    data += ser_compact_size(0)               # contributions.blobs
    data += bytes(BLS_SIG_SIZE)               # sig
    return data


class LLMQDKGNonMemberRelay(KerriganTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_kerrigan_test_params(4, 3, [["-whitelist=127.0.0.1"]] * 4)

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        llmq_type_name = "llmq_test"
        mninfos = self.mninfo.copy()
        all_nodes = [self.nodes[0]] + [mn.get_node(self) for mn in mninfos]
        target = mninfos[0].get_node(self)

        self.log.info("Move to the next DKG boundary")
        llmq_cycle_len = 24
        skip_count = llmq_cycle_len - (self.nodes[0].getblockcount() % llmq_cycle_len)
        if skip_count != 0:
            self.bump_mocktime(1)
            self.generate(self.nodes[0], skip_count, sync_fun=lambda: self.sync_blocks(all_nodes))
        q = self.nodes[0].getbestblockhash()

        self.wait_for_quorum_phase(q, 1, 3, None, 0, mninfos, llmq_type_name=llmq_type_name)
        self.wait_for_quorum_connections(q, 2, mninfos, wait_proc=lambda: self.bump_mocktime(1),
                                         llmq_type_name=llmq_type_name)
        self.move_blocks(all_nodes, 2)
        self.wait_for_quorum_phase(q, 2, 3, "receivedContributions", 3, mninfos,
                                   llmq_type_name=llmq_type_name)

        self.log.info("A contribution from a non-member is dropped without punishing the relayer")
        stranger = os.urandom(32).hex()
        relayer = target.add_p2p_connection(P2PInterface())
        # "skipping" is logged after the ban decision for the same message, so
        # once it appears a ban line would already have been written.
        with target.assert_debug_log(
                expected_msgs=["contributor %s not a member of this quorum" % stranger,
                               "skipping message due to failed preverification"],
                unexpected_msgs=["banning node due to failed preverification"],
                timeout=30):
            relayer.send_message(msg_qcontrib_raw(nonmember_contribution(q, stranger)))
        assert relayer.is_connected
        target.disconnect_p2ps()

        self.log.info("Control: a malformed contribution is still punished")
        sender = target.add_p2p_connection(P2PInterface())
        with target.assert_debug_log(
                expected_msgs=["failed to deserialize message", "DISCOURAGE THRESHOLD EXCEEDED"],
                timeout=30):
            sender.send_message(msg_qcontrib_raw(nonmember_contribution(q, os.urandom(32).hex())[:40]))
        target.disconnect_p2ps()


if __name__ == "__main__":
    LLMQDKGNonMemberRelay().main()
