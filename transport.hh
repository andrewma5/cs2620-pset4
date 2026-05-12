#pragma once
// pset4/transport.hh - inter-replica message transport for the paxos layer.
//
// pt_paxos_replica uses a `transport<paxos_message>*` for all peer-to-peer
// communication. Two implementations:
//   - sim_transport<T>: wraps netsim. Used by tier-1 sim tests.
//   - tcp_transport: real TCP with length-prefixed JSON frames. Used by
//     tm-server in multi-replica production runs. (Phase 2.3b.)
//
// Replicas don't talk to clients through the transport in pset4 — that's
// the HTTP layer's job (tm-server.cc), upstream of paxos.

#include "netsim.hh"
#include "random_source.hh"
#include "cotamer/cotamer.hh"
#include <cstddef>
#include <format>
#include <memory>
#include <vector>

namespace tmgr {

namespace cot = cotamer;

// Abstract transport for inter-replica messages of type Msg.
// `send(peer, msg)` sends to a specific peer index; `receive()` returns the
// next message from any peer.
template <typename Msg>
class transport {
public:
    virtual ~transport() = default;
    virtual cot::task<Msg> receive() = 0;
    virtual cot::task<> send(size_t peer, Msg msg) = 0;
    virtual size_t nreplicas() const = 0;
};

// In-process simulator transport wrapping netsim. Phase-3 tier-1 tests
// construct N of these (one per replica) and wire them pairwise via
// `connect(peer, peer_transport)`.
template <typename Msg>
class sim_transport : public transport<Msg> {
public:
    sim_transport(size_t my_index, size_t nreplicas, random_source& rnd)
        : my_index_(my_index),
          nreplicas_(nreplicas),
          port_(rnd, std::format("R{}", my_index)) {
        channels_.reserve(nreplicas);
        for (size_t i = 0; i < nreplicas; ++i) {
            channels_.emplace_back(std::make_unique<netsim::channel<Msg>>(
                rnd, std::format("R{}->R{}", my_index, i)));
        }
    }

    // Wire this transport's outgoing channel to peer's incoming port.
    // Caller is responsible for the full mesh after all transports exist.
    void connect(size_t peer, sim_transport<Msg>& peer_transport) {
        channels_[peer]->connect(peer_transport.port_);
    }

    cot::task<Msg> receive() override { return port_.receive(); }
    cot::task<> send(size_t peer, Msg msg) override {
        return channels_[peer]->send(std::move(msg));
    }
    size_t nreplicas() const override { return nreplicas_; }

    // Exposed for failure-injection tests (set_loss, etc.).
    netsim::port<Msg>& port() { return port_; }
    netsim::channel<Msg>& channel_to(size_t peer) { return *channels_[peer]; }

private:
    size_t my_index_;
    size_t nreplicas_;
    netsim::port<Msg> port_;
    std::vector<std::unique_ptr<netsim::channel<Msg>>> channels_;
};

}  // namespace tmgr
