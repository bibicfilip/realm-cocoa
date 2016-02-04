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

#include "impl/async_query.hpp"

#include "impl/realm_coordinator.hpp"
#include "results.hpp"

using namespace realm;
using namespace realm::_impl;

AsyncQuery::AsyncQuery(Results& target)
: CallbackCollection(target.get_realm().shared_from_this())
, m_target_results(&target)
, m_sort(target.get_sort())
{
    Query q = target.get_query();
    m_query_handover = Realm::Internal::get_shared_group(get_realm()).export_for_handover(q, MutableSourcePayload::Move);
}

void AsyncQuery::release_data() noexcept
{
    m_query = nullptr;
}

// Most of the inter-thread synchronization for run(), prepare_handover(),
// attach_to(), detach(), release_query() and deliver() is done by
// RealmCoordinator external to this code, which has some potentially
// non-obvious results on which members are and are not safe to use without
// holding a lock.
//
// attach_to(), detach(), run(), prepare_handover(), and release_query() are
// all only ever called on a single thread. call_callbacks() and deliver() are
// called on the same thread. Calls to prepare_handover() and deliver() are
// guarded by a lock.
//
// In total, this means that the safe data flow is as follows:
//  - prepare_handover(), attach_to(), detach() and release_query() can read
//    members written by each other
//  - deliver() can read members written to in prepare_handover(), deliver(),
//    and call_callbacks()
//  - call_callbacks() and read members written to in deliver()
//
// Separately from this data flow for the query results, all uses of
// m_target_results, m_callbacks, and m_callback_index must be done with the
// appropriate mutex held to avoid race conditions when the Results object is
// destroyed while the background work is running, and to allow removing
// callbacks from any thread.

static bool map_moves(size_t& idx, TableChangeInfo const& changes)
{
    auto it = changes.moves.find(idx);
    if (it != changes.moves.end()) {
        idx = it->second;
        return true;
    }
    return false;
}

static bool row_did_change(Table& table, size_t idx, std::vector<TableChangeInfo> const& modified, int depth = 0)
{
    if (depth > 16)  // arbitrary limit
        return false;

    size_t table_ndx = table.get_index_in_group();
    if (table_ndx < modified.size()) {
        auto const& changes = modified[table_ndx];
        map_moves(idx, changes);
        if (changes.changes.contains(idx))
            return true;
    }

    for (size_t i = 0, count = table.get_column_count(); i < count; ++i) {
        auto type = table.get_column_type(i);
        if (type == type_Link) {
            auto& target = *table.get_link_target(i);
            auto dst = table.get_link(i, idx);
            return row_did_change(target, dst, modified, depth + 1);
        }
        if (type != type_LinkList)
            continue;

        auto& target = *table.get_link_target(i);
        auto lvr = table.get_linklist(i, idx);
        for (size_t j = 0; j < lvr->size(); ++j) {
            size_t dst = lvr->get(j).get_index();
            if (row_did_change(target, dst, modified, depth + 1))
                return true;
        }
    }

    return false;
}

namespace {

struct RowInfo {
    size_t shifted_row_index;
    size_t prev_tv_index;
    size_t tv_index;
};

class pos_iterator {
public:
    pos_iterator(IndexSet::const_iterator it) : m_iterator(it) { }
    size_t operator*() const { return m_iterator->first + m_offset; }
    bool operator!=(IndexSet::const_iterator const& it) const { return m_iterator != it; }

    pos_iterator& operator++()
    {
        ++m_offset;
        if (m_iterator->first + m_offset == m_iterator->second) {
            ++m_iterator;
            m_offset = 0;
        }
        return *this;
    }

private:
    IndexSet::const_iterator m_iterator;
    size_t m_offset = 0;
};

void calculate_moves_unsorted(std::vector<RowInfo>& new_rows, CollectionChangeIndices& changeset, Table& table,
                              std::vector<TableChangeInfo> const& modified_rows)
{
    std::sort(begin(new_rows), end(new_rows), [](auto& lft, auto& rgt) {
        return lft.tv_index < rgt.tv_index;
    });

    pos_iterator ins = changeset.insertions.begin(), del = changeset.deletions.begin();
    int shift = 0;
    for (auto& row : new_rows) {
        while (del != changeset.deletions.end() && *del <= row.tv_index) {
            ++del;
            ++shift;
        }
        while (ins != changeset.insertions.end() && *ins <= row.tv_index) {
            ++ins;
            --shift;
        }
        if (row.prev_tv_index == npos)
            continue;

        if (row_did_change(table, row.shifted_row_index, modified_rows))
            changeset.modifications.add(row.tv_index);
        if (row.tv_index + shift != row.prev_tv_index) {
            --shift;
            changeset.moves.push_back({row.prev_tv_index, row.tv_index});
        }
    }
}

using items = std::vector<std::pair<size_t, size_t>>;

struct Match {
    size_t i, j, size;
};

Match find_longest_match(items const& a, items const& b,
                         size_t begin1, size_t end1, size_t begin2, size_t end2)
{
    Match best = {begin1, begin2, 0};
    std::vector<size_t> len_from_j;
    len_from_j.resize(end2 - begin2, 0);
    std::vector<size_t> len_from_j_prev = len_from_j;

    for (size_t i = begin1; i < end1; ++i) {
        std::fill(begin(len_from_j), end(len_from_j), 0);

        size_t ai = a[i].first;
        auto it = lower_bound(begin(b), end(b), std::make_pair(size_t(0), ai),
                              [](auto a, auto b) { return a.second < b.second; });
        for (; it != end(b) && it->second == ai; ++it) {
            size_t j = it->first;
            if (j < begin2)
                continue;
            if (j >= end2)
                break;

            size_t off = j - begin1;
            size_t size = off == 0 ? 1 : len_from_j_prev[off - 1] + 1;
            len_from_j[off] = size;
            if (size > best.size) {
                best.i = i - size + 1;
                best.j = j - size + 1;
                best.size = size;
            }
        }
        len_from_j.swap(len_from_j_prev);
    }
    return best;
}

void find_longest_matches(items const& a, items const& b_ndx,
                          size_t begin1, size_t end1, size_t begin2, size_t end2, std::vector<Match>& ret)
{
    // FIXME: recursion could get too deep here
    Match m = find_longest_match(a, b_ndx, begin1, end1, begin2, end2);
    if (!m.size)
        return;
    if (m.i > begin1 && m.j > begin2)
        find_longest_matches(a, b_ndx, begin1, m.i, begin2, m.j, ret);
    ret.push_back(m);
    if (m.i + m.size < end2 && m.j + m.size < end2)
        find_longest_matches(a, b_ndx, m.i + m.size, end1, m.j + m.size, end2, ret);
}

void calculate_moves_sorted(std::vector<RowInfo>& new_rows, CollectionChangeIndices& changeset, Table& table,
                            std::vector<TableChangeInfo> const& modified_rows)
{
    std::vector<std::pair<size_t, size_t>> old_candidates;
    std::vector<std::pair<size_t, size_t>> new_candidates;

    std::sort(begin(new_rows), end(new_rows), [](auto& lft, auto& rgt) {
        return lft.tv_index < rgt.tv_index;
    });

    pos_iterator ins = changeset.insertions.begin(), del = changeset.deletions.begin();
    int shift = 0;
    for (auto& row : new_rows) {
        while (del != changeset.deletions.end() && *del <= row.tv_index) {
            ++del;
            ++shift;
        }
        while (ins != changeset.insertions.end() && *ins <= row.tv_index) {
            ++ins;
            --shift;
        }
        if (row.prev_tv_index == npos)
            continue;

        if (row_did_change(table, row.shifted_row_index, modified_rows)) {
            changeset.modifications.add(row.tv_index);
        }
            old_candidates.push_back({row.shifted_row_index, row.prev_tv_index});
            new_candidates.push_back({row.shifted_row_index, row.tv_index});
//        }
    }

    std::sort(begin(old_candidates), end(old_candidates), [](auto a, auto b) {
        if (a.second != b.second)
            return a.second < b.second;
        return a.first < b.first;
    });

    // First check if the order of any of the rows actually changed
    size_t first_difference = npos;
    for (size_t i = 0; i < old_candidates.size(); ++i) {
        if (old_candidates[i].first != new_candidates[i].first) {
            first_difference = i;
            break;
        }
    }
    if (first_difference == npos)
        return;

    const auto b_ndx = [&]{
        std::vector<std::pair<size_t, size_t>> ret;
        ret.reserve(new_candidates.size());
        for (size_t i = 0; i < new_candidates.size(); ++i)
            ret.push_back(std::make_pair(i, new_candidates[i].first));
        std::sort(begin(ret), end(ret), [](auto a, auto b) {
            if (a.second != b.second)
                return a.second < b.second;
            return a.first < b.first;
        });
        return ret;
    }();

    std::vector<Match> longest_matches;
    find_longest_matches(old_candidates, b_ndx,
                         first_difference, old_candidates.size(),
                         first_difference, new_candidates.size(),
                         longest_matches);
    longest_matches.push_back({old_candidates.size(), new_candidates.size(), 0});

    size_t i = first_difference, j = first_difference;
    for (auto match : longest_matches) {
        for (; i < match.i; ++i)
            changeset.deletions.add(old_candidates[i].second);
        for (; j < match.j; ++j)
            changeset.insertions.add(new_candidates[j].second);
        i += match.size;
        j += match.size;
    }
}
}

CollectionChangeIndices AsyncQuery::calculate_changes(size_t table_ndx,
                                                      std::vector<TableChangeInfo> const& modified_rows)
{
    auto changes = table_ndx < modified_rows.size() ? &modified_rows[table_ndx] : nullptr;

    auto do_calculate_changes = [&](auto& old_rows, auto& new_rows) {
        CollectionChangeIndices changeset;
        size_t i = 0, j = 0;
        while (i < old_rows.size() && j < new_rows.size()) {
            auto old_index = old_rows[i];
            auto new_index = new_rows[j];
            if (old_index.shifted_row_index == new_index.shifted_row_index) {
                new_rows[j].prev_tv_index = old_rows[i].tv_index;
                ++i;
                ++j;
            }
            else if (old_index.shifted_row_index < new_index.shifted_row_index) {
                changeset.deletions.add(old_index.tv_index);
                ++i;
            }
            else {
                changeset.insertions.add(new_index.tv_index);
                ++j;
            }
        }

        for (; i < old_rows.size(); ++i)
            changeset.deletions.add(old_rows[i].tv_index);
        for (; j < new_rows.size(); ++j)
            changeset.insertions.add(new_rows[j].tv_index);

        if (m_sort) {
            calculate_moves_sorted(new_rows, changeset, *m_query->get_table(), modified_rows);
        }
        else {
            calculate_moves_unsorted(new_rows, changeset, *m_query->get_table(), modified_rows);
        }

        return changeset;
    };

    std::vector<RowInfo> old_rows;
    for (size_t i = 0; i < m_previous_rows.size(); ++i) {
        auto ndx = m_previous_rows[i];
        old_rows.push_back({ndx, npos, i});
    }
    std::stable_sort(begin(old_rows), end(old_rows), [](auto& lft, auto& rgt) {
        return lft.shifted_row_index < rgt.shifted_row_index;
    });

    std::vector<RowInfo> new_rows;
    for (size_t i = 0; i < m_tv.size(); ++i) {
        auto ndx = m_tv[i].get_index();
        if (changes)
            map_moves(ndx, *changes);
        new_rows.push_back({ndx, npos, i});
    }
    std::stable_sort(begin(new_rows), end(new_rows), [](auto& lft, auto& rgt) {
        return lft.shifted_row_index < rgt.shifted_row_index;
    });
    return do_calculate_changes(old_rows, new_rows);
}

void AsyncQuery::run(TransactionChangeInfo& info)
{
    m_did_change = false;

    {
        std::lock_guard<std::mutex> target_lock(m_target_mutex);
        // Don't run the query if the results aren't actually going to be used
        if (!m_target_results || (!have_callbacks() && !m_target_results->wants_background_updates())) {
            return;
        }
    }

    REALM_ASSERT(!m_tv.is_attached());

    size_t table_ndx = m_query->get_table()->get_index_in_group();

    // If we've run previously, check if we need to rerun
    if (m_initial_run_complete) {
        // Make an empty tableview from the query to get the table version, since
        // Query doesn't expose it
        if (m_query->find_all(0, 0, 0).outside_version() == m_handed_over_table_version) {
            return;
        }
    }

    m_tv = m_query->find_all();
    if (m_sort) {
        m_tv.sort(m_sort.column_indices, m_sort.ascending);
    }

    if (m_initial_run_complete) {
        m_new_changes = calculate_changes(table_ndx, info.tables);
        if (m_new_changes.empty()) {
            m_tv = {};
            return;
        }
    }

    m_did_change = true;

    m_previous_rows.clear();
    m_previous_rows.resize(m_tv.size());
    for (size_t i = 0; i < m_tv.size(); ++i)
        m_previous_rows[i] = m_tv[i].get_index();
}

bool AsyncQuery::do_prepare_handover(SharedGroup& sg)
{
    if (!m_tv.is_attached()) {
        return false;
    }

    REALM_ASSERT(m_tv.is_in_sync());

    m_initial_run_complete = true;
    m_handed_over_table_version = m_tv.outside_version();
    m_tv_handover = sg.export_for_handover(m_tv, MutableSourcePayload::Move);

    // FIXME: this is not actually correct
    // merge or something? double-calculate?
    m_changes = std::move(m_new_changes);
//    m_changes.insert(m_changes.end(), m_new_changes.begin(), m_new_changes.end());

    // detach the TableView as we won't need it again and keeping it around
    // makes advance_read() much more expensive
    m_tv = {};

    return m_did_change;
}

bool AsyncQuery::deliver(SharedGroup& sg, std::exception_ptr err, TransactionChangeInfo&)
{
    if (!is_for_current_thread()) {
        return false;
    }

    std::lock_guard<std::mutex> target_lock(m_target_mutex);

    // Target results being null here indicates that it was destroyed while we
    // were in the process of advancing the Realm version and preparing for
    // delivery, i.e. it was destroyed from the "wrong" thread
    if (!m_target_results) {
        return false;
    }

    // We can get called before the query has actually had the chance to run if
    // we're added immediately before a different set of async results are
    // delivered
    if (!m_initial_run_complete && !err) {
        return false;
    }

    if (err) {
        set_error(err);
        return have_callbacks();
    }

    REALM_ASSERT(!m_query_handover);

    auto realm_sg_version = sg.get_version_of_current_transaction();
    if (version() != realm_sg_version) {
        // Realm version can be newer if a commit was made on our thread or the
        // user manually called refresh(), or older if a commit was made on a
        // different thread and we ran *really* fast in between the check for
        // if the shared group has changed and when we pick up async results
        return false;
    }

    set_change(std::move(m_changes));
    if (m_tv_handover) {
        m_tv_handover->version = version();
        Results::Internal::set_table_view(*m_target_results,
                                          std::move(*sg.import_from_handover(std::move(m_tv_handover))));
    }
    REALM_ASSERT(!m_tv_handover);
    return have_callbacks();
}

void AsyncQuery::do_attach_to(SharedGroup& sg)
{
    REALM_ASSERT(m_query_handover);
    m_query = sg.import_from_handover(std::move(m_query_handover));
}

void AsyncQuery::do_detach_from(SharedGroup& sg)
{
    REALM_ASSERT(m_query);
    REALM_ASSERT(!m_tv.is_attached());

    m_query_handover = sg.export_for_handover(*m_query, MutableSourcePayload::Move);
    m_query = nullptr;
}
