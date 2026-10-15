#pragma once

#include "core/logger.hpp"

#include <cstdint>
#include <exception>
#include <typeinfo>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::core {

/// Notifications that something happened: the player became a ghost, another
/// player gained a level. One publisher, any number of subscribers, and
/// neither knows the other - a subscriber includes the event's header, not the
/// class that raises it.
///
/// Only for notifications. A question the game asks the rest of the client
/// ("what is this spell's icon") is an interface passed in; an order the
/// interface gives the game ("sell this item") is a method on whoever owns
/// that state. See AGENTS.md, "Events".
///
/// Threads: subscribe, publish and dispatchPosted belong to the main thread.
/// post may be called from any thread; what it queues is delivered on the
/// main thread by the next dispatchPosted.
///
/// Lifetime: the bus outlives every Subscription taken from it. Application
/// owns it and declares it before everything that subscribes.
class EventBus {
public:
    /// Ends a subscription when it goes out of scope. Keep it as a member of
    /// whatever the handler captures, so the two die together.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept { *this = std::move(other); }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                bus_ = std::exchange(other.bus_, nullptr);
                key_ = other.key_;
                id_ = other.id_;
            }
            return *this;
        }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { reset(); }

        void reset() {
            if (auto* bus = std::exchange(bus_, nullptr)) bus->unsubscribe(key_, id_);
        }

        /// Whether this still holds a subscription.
        explicit operator bool() const { return bus_ != nullptr; }

    private:
        friend class EventBus;
        Subscription(EventBus* bus, const void* key, uint64_t id) : bus_(bus), key_(key), id_(id) {}
        EventBus* bus_ = nullptr;
        const void* key_ = nullptr;
        uint64_t id_ = 0;
    };

    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    template <class Event>
    [[nodiscard]] Subscription subscribe(std::function<void(const Event&)> handler) {
        const uint64_t id = ++lastId_;
        channel<Event>().slots.push_back({id, std::move(handler), true});
        return Subscription(this, key<Event>(), id);
    }

    /// Deliver to every subscriber now, before returning. Subscribers added
    /// while it runs do not see this event; ones removed while it runs are
    /// not called again.
    template <class Event>
    void publish(const Event& event) {
        auto it = channels_.find(key<Event>());
        if (it == channels_.end()) return;
        static_cast<Channel<Event>&>(*it->second).deliver(event);
    }

    /// Queue for the next dispatchPosted. Any thread.
    template <class Event>
    void post(Event event) {
        std::lock_guard<std::mutex> lock(postedMutex_);
        posted_.push_back([this, e = std::move(event)] { publish(e); });
    }

    /// Deliver everything posted so far, in order. Main thread, once a frame.
    /// Events posted by these handlers wait for the next call.
    void dispatchPosted() {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(postedMutex_);
            batch.swap(posted_);
        }
        for (auto& deliver : batch) deliver();
    }

private:
    struct ChannelBase {
        virtual ~ChannelBase() = default;
        virtual void remove(uint64_t id) = 0;
    };

    template <class Event>
    struct Channel final : ChannelBase {
        struct Slot {
            uint64_t id;
            std::function<void(const Event&)> handler;
            // Cleared rather than the handler destroyed: a handler that ends
            // its own subscription is still running when it does.
            bool live = true;
            bool exceptionLogged = false;
        };
        // A deque, so a handler that subscribes while it runs does not move
        // the handler that is running.
        std::deque<Slot> slots;
        int delivering = 0;
        bool hasRemoved = false;

        void deliver(const Event& event) {
            struct DeliveryGuard {
                Channel& channel;
                explicit DeliveryGuard(Channel& c) : channel(c) { ++channel.delivering; }
                ~DeliveryGuard() {
                    if (--channel.delivering == 0 && channel.hasRemoved) channel.compact();
                }
            } guard(*this);
            const size_t count = slots.size();
            for (size_t i = 0; i < count; ++i) {
                if (!slots[i].live) continue;
                try {
                    slots[i].handler(event);
                } catch (const std::exception& e) {
                    if (!std::exchange(slots[i].exceptionLogged, true)) {
                        LOG_ERROR("EventBus: subscriber ", slots[i].id, " for ", typeid(Event).name(),
                                  " threw: ", e.what(), "; continuing delivery to remaining subscribers");
                    }
                } catch (...) {
                    if (!std::exchange(slots[i].exceptionLogged, true)) {
                        LOG_ERROR("EventBus: subscriber ", slots[i].id, " for ", typeid(Event).name(),
                                  " threw an unknown exception; continuing delivery to remaining subscribers");
                    }
                }
            }
        }

        void remove(uint64_t id) override {
            for (auto& slot : slots) {
                if (slot.id == id) slot.live = false;
            }
            hasRemoved = true;
            if (delivering == 0) compact();
        }

        void compact() {
            // Destroy removed captures only after slots is consistent. A capture
            // may own another Subscription whose destructor calls remove again.
            std::deque<Slot> kept;
            for (auto& slot : slots) {
                if (slot.live) kept.push_back(std::move(slot));
            }
            slots.swap(kept);
            hasRemoved = false;
        }
    };

    template <class Event>
    static const void* key() {
        static const char tag = 0;
        return &tag;
    }

    template <class Event>
    Channel<Event>& channel() {
        auto& slot = channels_[key<Event>()];
        if (!slot) slot = std::make_unique<Channel<Event>>();
        return static_cast<Channel<Event>&>(*slot);
    }

    void unsubscribe(const void* key, uint64_t id) {
        auto it = channels_.find(key);
        if (it != channels_.end()) it->second->remove(id);
    }

    std::unordered_map<const void*, std::unique_ptr<ChannelBase>> channels_;
    uint64_t lastId_ = 0;
    std::mutex postedMutex_;
    std::vector<std::function<void()>> posted_;
};

} // namespace wowee::core
