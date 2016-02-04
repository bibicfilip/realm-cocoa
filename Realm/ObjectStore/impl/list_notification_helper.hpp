////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
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

#ifndef REALM_LIST_NOTIFICATION_HELPER_HPP
#define REALM_LIST_NOTIFICATION_HELPER_HPP

#include "impl/callback_collection.hpp"

#include <realm/group_shared.hpp>

namespace realm {
namespace _impl {
class ListNotificationHelper : public CallbackCollection {
public:
    ListNotificationHelper(LinkViewRef lv, std::shared_ptr<Realm> realm);

private:
    LinkViewRef m_lv;
    std::unique_ptr<SharedGroup::Handover<LinkView>> m_lv_handover;

    bool deliver(SharedGroup& sg, std::exception_ptr err, TransactionChangeInfo& info) override;
    bool do_prepare_handover(SharedGroup&) override { return true; }

    void do_attach_to(SharedGroup& sg) override;
    void do_detach_from(SharedGroup& sg) override;

    void release_data() noexcept override;
    void add_required_change_info(TransactionChangeInfo& info) override;
};
}
}

#endif // REALM_LIST_NOTIFICATION_HELPER_HPP
