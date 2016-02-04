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

#include "collection_notifications.hpp"

#include "impl/callback_collection.hpp"

using namespace realm;

NotificationToken::NotificationToken(std::shared_ptr<_impl::CallbackCollection> query, size_t token)
: m_query(std::move(query)), m_token(token)
{
}

NotificationToken::~NotificationToken()
{
    // m_query itself (and not just the pointed-to thing) needs to be accessed
    // atomically to ensure that there are no data races when the token is
    // destroyed after being modified on a different thread.
    // This is needed despite the token not being thread-safe in general as
    // users find it very surpringing for obj-c objects to care about what
    // thread they are deallocated on.
    if (auto query = std::atomic_load(&m_query)) {
        query->remove_callback(m_token);
    }
}

NotificationToken::NotificationToken(NotificationToken&& rgt)
: m_query(std::atomic_exchange(&rgt.m_query, {})), m_token(rgt.m_token)
{
}

NotificationToken& NotificationToken::operator=(realm::NotificationToken&& rgt)
{
    if (this != &rgt) {
        if (auto query = std::atomic_load(&m_query)) {
            query->remove_callback(m_token);
        }
        std::atomic_store(&m_query, std::atomic_exchange(&rgt.m_query, {}));
        m_token = rgt.m_token;
    }
    return *this;
}
