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

#include "impl/list_notification_helper.hpp"

#include "impl/realm_coordinator.hpp"
#include "shared_realm.hpp"

#include <realm/link_view.hpp>

using namespace realm;
using namespace realm::_impl;

ListNotificationHelper::ListNotificationHelper(LinkViewRef lv, std::shared_ptr<Realm> realm)
: CallbackCollection(std::move(realm))
{
    auto& sg = Realm::Internal::get_shared_group(get_realm());
    m_lv_handover = sg.export_linkview_for_handover(lv);
}

void ListNotificationHelper::release_data() noexcept
{
    // FIXME: does this need a lock?
    m_lv.reset();
}

void ListNotificationHelper::do_attach_to(SharedGroup& sg)
{
    REALM_ASSERT(m_lv_handover);
    m_lv = sg.import_linkview_from_handover(std::move(m_lv_handover));
}

void ListNotificationHelper::do_detach_from(SharedGroup& sg)
{
    REALM_ASSERT(!m_lv_handover);
    m_lv_handover = sg.export_linkview_for_handover(m_lv);
}

void ListNotificationHelper::add_required_change_info(TransactionChangeInfo& info)
{
    REALM_ASSERT(m_lv && !m_lv_handover);
    size_t row_ndx = m_lv->get_origin_row_index();
    size_t col_ndx = not_found;

    auto& table = m_lv->get_origin_table();
    for (size_t i = 0, count = table.get_column_count(); i != count; ++i) {
        if (table.get_column_type(i) == type_LinkList && table.get_linklist(i, row_ndx) == m_lv) {
            col_ndx = i;
            break;
        }
    }
    REALM_ASSERT(col_ndx != not_found);

    info.lists.push_back({table.get_index_in_group(), row_ndx, col_ndx});
}

bool ListNotificationHelper::deliver(SharedGroup& sg,
                                     std::exception_ptr err,
                                     TransactionChangeInfo& info)
{
//    if (!is_for_current_thread()) {
//        return false;
//    }

    if (err) {
        set_error(err);
        return have_callbacks();
    }

    auto realm_sg_version = sg.get_version_of_current_transaction();
    if (version() != realm_sg_version) {
        return false;
    }

    // FIXME: push everything above into base class

    if (!have_callbacks())
        return false;

    REALM_ASSERT(m_lv);
    for (auto& list : info.lists) {
        if (list.table_ndx != m_lv->get_origin_table().get_index_in_group())
            continue;
        if (list.row_ndx != m_lv->get_origin_row_index())
            continue;
        if (m_lv->get_origin_table().get_linklist(list.col_ndx, list.row_ndx) != m_lv)
            continue;

        CollectionChangeIndices change;
        change.insertions = list.inserts;
        change.deletions = list.deletes;
        change.modifications = list.changes;
        for (auto& move : list.moves)
            change.moves.push_back({move.first, move.second});
        set_change(std::move(change));
        return true;
    }

    return false;
}
