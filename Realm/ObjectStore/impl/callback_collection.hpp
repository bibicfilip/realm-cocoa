////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#ifndef REALM_CALLBACK_COLLECTION_HPP
#define REALM_CALLBACK_COLLECTION_HPP

#include "collection_notifications.hpp"

#include <realm/group_shared.hpp>

#include <mutex>
#include <functional>
#include <set>
#include <thread>

namespace realm {
class Realm;

namespace _impl {
struct TransactionChangeInfo;

class CallbackCollection {
public:
    CallbackCollection(std::shared_ptr<Realm>);
    virtual ~CallbackCollection();
    void unregister() noexcept;

    virtual void release_data() noexcept = 0;

    size_t add_callback(CollectionChangeCallback callback);
    void remove_callback(size_t token);

    void call_callbacks();

    bool is_alive() const noexcept;

    Realm& get_realm() const noexcept { return *m_realm; }

    // Attach the handed-over query to `sg`
    void attach_to(SharedGroup& sg);
    // Create a new query handover object and stop using the previously attached
    // SharedGroup
    void detach();

    virtual void add_required_change_info(TransactionChangeInfo&) { }

    virtual void run(TransactionChangeInfo&) { }
    void prepare_handover();
    virtual bool deliver(SharedGroup&, std::exception_ptr, TransactionChangeInfo& info) = 0;

    // Get the version of the current handover object
    SharedGroup::VersionID version() const noexcept { return m_sg_version; }

protected:
    bool have_callbacks() const noexcept { return m_have_callbacks; }

    void set_error(std::exception_ptr err) { m_error = err; }
    void set_change(CollectionChangeIndices change) { m_change = std::move(change); }

private:
    virtual void do_attach_to(SharedGroup&) = 0;
    virtual void do_detach_from(SharedGroup&) = 0;
    virtual bool do_prepare_handover(SharedGroup&) = 0;

    mutable std::mutex m_realm_mutex;
    std::shared_ptr<Realm> m_realm;

    SharedGroup::VersionID m_sg_version;
    SharedGroup* m_sg = nullptr;

    std::exception_ptr m_error;
    CollectionChangeIndices m_change;

    uint_fast64_t m_results_version = 0;

    struct Callback {
        CollectionChangeCallback fn;
        size_t token;
        uint_fast64_t delivered_version;
    };

    // Currently registered callbacks and a mutex which must always be held
    // while doing anything with them or m_callback_index
    std::mutex m_callback_mutex;
    std::vector<Callback> m_callbacks;

    // Cached value for if m_callbacks is empty, needed to avoid deadlocks in
    // run() due to lock-order inversion between m_callback_mutex and m_target_mutex
    // It's okay if this value is stale as at worst it'll result in us doing
    // some extra work.
    std::atomic<bool> m_have_callbacks = {false};

    // Iteration variable for looping over callbacks
    // remove_callback() updates this when needed
    size_t m_callback_index = npos;

    CollectionChangeCallback next_callback();
};

} // namespace _impl
} // namespace realm

#endif /* REALM_CALLBACK_COLLECTION_HPP */
