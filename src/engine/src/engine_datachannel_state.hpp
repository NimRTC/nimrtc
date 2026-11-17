/**
 * @file src/engine/src/engine_datachannel_state.hpp
 * @brief PRIVATE engine collaborator: the data-channel statistics state
 *        (plan Track S / S6).
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What the state is
 *
 * The engine's account of the **data channels the host opened**.  Two
 * halves live here:
 *
 *   1. the **identity** of each channel — the engine-assigned id, the
 *      label the host gave it, the reliability mode it was created
 *      with, and the last state the backend reported;
 *   2. the **inbound counters** — how many messages and bytes arrived
 *      on each channel.
 *
 * ## Why the inbound half is the engine's
 *
 * The engine installs the channel's callbacks, so it sees every
 * inbound message.  Counting them there is exact and costs nothing:
 * the callback is already running.
 *
 * ## Why the outbound half is NOT here
 *
 * The *outbound* counters — how many messages the backend handed to
 * the transport, how many a partial-reliability policy dropped, how
 * many it retransmitted — are the **backend's** account.  The engine
 * cannot derive them: it only sees the `send()` call, and what
 * happens to that message afterwards (queued, retransmitted, dropped
 * when its lifetime expired) happens inside the backend.
 *
 * So the engine asks the backend, through the additive
 * `plugins::IDataChannel::stats()` seam, and republishes the answer.
 * A backend that does not implement the seam reports zeroes — and the
 * engine says so in the entry, rather than inventing a number.
 *
 * ## The proxy — why the engine wraps the backend at all
 *
 * `create_data_channel()` hands the channel to the host as a
 * `std::unique_ptr<IDataChannel>`, so the *host* owns the backend
 * instance.  The engine cannot keep a raw pointer to it: the host is
 * free to drop the channel at any moment, and the engine still has to
 * publish statistics for it (the entry must keep reporting the last
 * known numbers rather than dangling).
 *
 * The engine therefore keeps a `std::shared_ptr` to the backend and
 * hands the host a **proxy** — a thin `IDataChannel` implementation
 * that forwards every call to the backend and, on the way, counts the
 * inbound messages and records the last reported state.
 *
 * The proxy is also what makes the inbound counting possible at all:
 * the host may never install a message callback, and the engine still
 * has to count.  The proxy's own callback is always installed on the
 * backend.
 *
 * ## The lifetime rule
 *
 * The backend may outlive the proxy (the engine keeps it alive for
 * the statistics), so the lambdas the proxy installs on the backend
 * must **not** capture the proxy's `this`.  They capture a shared
 * "callback block" instead, so a late inbound message finds live
 * state rather than a dangling pointer.
 *
 * ## Threading
 *
 * A backend may fire its callbacks from its own thread while the
 * host polls `stats()` from another one.  The per-channel counters are
 * therefore atomic, and the host's callbacks are guarded by a mutex.
 * The engine's *list* of channels is guarded by the state's own mutex:
 * it changes only when the host opens a channel, but `stats()` may run
 * from any thread.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nimrtc/plugins/datachannel.hpp>

namespace nimrtc::engine::detail {

/**
 * @brief The canonical name of a reliability mode.
 *
 * The engine publishes the mode as a *name* rather than as an enum, so
 * a host that reads the statistics does not have to know the enum's
 * numeric values (which are not part of any stable contract).
 */
inline const char* datachannel_reliability_name(
    plugins::DataChannelReliability r) noexcept {
    switch (r) {
        case plugins::DataChannelReliability::kReliableOrdered:
            return "reliable-ordered";
        case plugins::DataChannelReliability::kUnreliable:
            return "unreliable";
        case plugins::DataChannelReliability::kPartialReliableTTL:
            return "partial-reliable-ttl";
        case plugins::DataChannelReliability::kPartialReliableCount:
            return "partial-reliable-count";
    }
    return "unknown";
}

/**
 * @brief The per-channel facts the engine publishes.
 *
 * Shared between the engine's entry and the proxy: the host may drop
 * the channel (and with it the proxy) at any moment, and the engine's
 * statistics entry must survive that — so the numbers live in a
 * `shared_ptr`-held struct rather than in either party.
 */
struct DataChannelShared {
    /** The engine-assigned channel id (1-based, monotonically
     *  increasing per engine). */
    std::uint32_t channel_id = 0;

    /** The channel's label (the identity the host gave it). */
    std::string label;

    /** The reliability mode the channel was created with. */
    plugins::DataChannelReliability reliability =
        plugins::DataChannelReliability::kReliableOrdered;

    /** The messages the engine saw arrive on this channel. */
    std::atomic<std::uint64_t> messages_received{0};

    /** The bytes the engine saw arrive on this channel. */
    std::atomic<std::uint64_t> bytes_received{0};

    /** The last state the backend reported ("connecting", "open",
     *  "closing", "closed", "error").  Empty when the backend never
     *  reported one. */
    mutable std::mutex state_mutex;
    std::string state;

    /** Record the channel's last reported state. */
    void set_state(const char* s) noexcept {
        if (s == nullptr) return;
        std::lock_guard<std::mutex> lock(state_mutex);
        state = s;
    }

    /** Read the channel's last reported state. */
    std::string get_state() const {
        std::lock_guard<std::mutex> lock(state_mutex);
        return state;
    }
};

/**
 * @brief The host's callbacks, shared between the proxy and the
 *        lambdas the proxy installs on the backend.
 *
 * The backend may outlive the proxy, so the installed lambdas capture
 * this block (not the proxy).  Every field is guarded by the mutex:
 * the host may install a callback from one thread while the backend
 * fires one from another.
 */
struct DataChannelCallbacks {
    mutable std::mutex mutex;
    plugins::DataMessageCallback on_message;
    plugins::DataChannelMessageCallback on_message_ex;
    plugins::DataChannelStateCallback on_state;
};

/**
 * @brief The engine's proxy around a backend data channel.
 *
 * A thin forwarder: every `IDataChannel` call goes to the backend
 * unchanged, and the *inbound* traffic passes through the engine's
 * own accounting on the way.  The proxy is what the host receives
 * from `create_data_channel()`.
 */
class DataChannelProxy final : public plugins::IDataChannel {
public:
    /**
     * @param inner   The backend instance (kept alive by the proxy).
     * @param shared  The per-channel facts the engine publishes.
     * @param cbs     The host's callbacks (shared with the lambdas
     *                installed on the backend).
     */
    DataChannelProxy(std::shared_ptr<plugins::IDataChannel> inner,
                     std::shared_ptr<DataChannelShared> shared,
                     std::shared_ptr<DataChannelCallbacks> cbs)
        : inner_(std::move(inner)), shared_(std::move(shared)),
          cbs_(std::move(cbs)) {
        install_backend_callbacks();
    }

    ~DataChannelProxy() override = default;

    // ---- IPlugin ----
    const char* name() const noexcept override {
        return inner_ ? inner_->name() : "datachannel";
    }
    plugins::Status open() noexcept override {
        return inner_ ? inner_->open() : plugins::kErrNotReady;
    }
    void close() noexcept override {
        if (inner_) inner_->close();
    }

    // ---- IDataChannel ----
    void open(const plugins::DataChannelConfig& cfg) override {
        if (inner_) inner_->open(cfg);
    }
    void send(plugins::BufferView data) override {
        if (inner_) inner_->send(data);
    }

    void set_on_message(plugins::DataMessageCallback cb) noexcept override {
        {
            std::lock_guard<std::mutex> lock(cbs_->mutex);
            cbs_->on_message = std::move(cb);
        }
        install_backend_callbacks();
    }

    void set_on_message_ex(
        plugins::DataChannelMessageCallback cb) noexcept override {
        {
            std::lock_guard<std::mutex> lock(cbs_->mutex);
            cbs_->on_message_ex = std::move(cb);
        }
        install_backend_callbacks();
    }

    void set_on_state(plugins::DataChannelStateCallback cb) noexcept override {
        {
            std::lock_guard<std::mutex> lock(cbs_->mutex);
            cbs_->on_state = std::move(cb);
        }
        install_backend_callbacks();
    }

    plugins::DataChannelStats stats() const noexcept override {
        return inner_ ? inner_->stats() : plugins::DataChannelStats{};
    }

private:
    /** Install the engine's counting callbacks on the backend.
     *
     * The lambdas capture the *shared* blocks, never `this`, so a
     * late inbound message cannot touch a destroyed proxy. */
    void install_backend_callbacks() noexcept {
        if (!inner_) return;

        auto shared = shared_;
        auto cbs = cbs_;

        inner_->set_on_message([shared, cbs](plugins::BufferView msg) {
            if (shared) {
                shared->messages_received.fetch_add(1, std::memory_order_relaxed);
                shared->bytes_received.fetch_add(msg.size(),
                                                 std::memory_order_relaxed);
            }
            plugins::DataMessageCallback cb;
            {
                std::lock_guard<std::mutex> lock(cbs->mutex);
                cb = cbs->on_message;
            }
            if (cb) {
                try {
                    cb(msg);
                } catch (...) {
                }
            }
        });

        inner_->set_on_message_ex(
            [cbs](const plugins::DataChannelMessage& msg) {
                plugins::DataChannelMessageCallback cb;
                {
                    std::lock_guard<std::mutex> lock(cbs->mutex);
                    cb = cbs->on_message_ex;
                }
                if (cb) {
                    try {
                        cb(msg);
                    } catch (...) {
                    }
                }
            });

        inner_->set_on_state([shared, cbs](const char* state) {
            if (shared) shared->set_state(state);
            plugins::DataChannelStateCallback cb;
            {
                std::lock_guard<std::mutex> lock(cbs->mutex);
                cb = cbs->on_state;
            }
            if (cb) {
                try {
                    cb(state);
                } catch (...) {
                }
            }
        });
    }

    /** The backend instance (shared with the engine's statistics
     *  entry, so the backend outlives the host's handle). */
    std::shared_ptr<plugins::IDataChannel> inner_;

    /** The per-channel facts (shared with the engine's statistics
     *  entry). */
    std::shared_ptr<DataChannelShared> shared_;

    /** The host's callbacks (shared with the installed lambdas). */
    std::shared_ptr<DataChannelCallbacks> cbs_;
};

/** One channel's entry in the engine's statistics state. */
struct DataChannelEntry {
    /** The per-channel facts (shared with the proxy). */
    std::shared_ptr<DataChannelShared> shared;

    /** The backend instance (shared with the proxy).  The engine keeps
     *  it alive so it can ask it for the outbound counters even after
     *  the host dropped its own handle. */
    std::shared_ptr<plugins::IDataChannel> backend;
};

/** The data-channel statistics state.
 *
 *  The list changes only when the host opens a channel, but `stats()`
 *  may run from any thread — so the list is guarded by a mutex. */
struct DataChannelState {
    /** One entry per channel the host opened, in creation order.
     *
     *  The order is the *creation* order and it is stable: a host
     *  that diffs two readings must not see the entries swap places,
     *  so the vector is append-only. */
    mutable std::mutex mutex;
    std::vector<DataChannelEntry> channels;

    /** Register a channel the host just opened.
     *
     *  @param channel_id  The engine-assigned id.
     *  @param label       The channel's label.
     *  @param reliability The channel's reliability mode.
     *  @param backend     The backend instance.
     *
     *  @return The shared facts of the new channel. */
    std::shared_ptr<DataChannelShared> add(
        std::uint32_t channel_id, std::string label,
        plugins::DataChannelReliability reliability,
        std::shared_ptr<plugins::IDataChannel> backend) {
        auto shared = std::make_shared<DataChannelShared>();
        shared->channel_id = channel_id;
        shared->label = std::move(label);
        shared->reliability = reliability;

        DataChannelEntry e;
        e.shared = shared;
        e.backend = std::move(backend);

        std::lock_guard<std::mutex> lock(mutex);
        channels.push_back(std::move(e));
        return shared;
    }
};

}  // namespace nimrtc::engine::detail
