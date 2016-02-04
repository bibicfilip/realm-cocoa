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

#include "index_set.hpp"

using namespace realm;

bool IndexSet::contains(size_t index) const
{
    auto it = const_cast<IndexSet*>(this)->find(index);
    return it != m_ranges.end() && it->first <= index;
}

IndexSet::iterator IndexSet::find(size_t index)
{
    for (auto it = m_ranges.begin(), end = m_ranges.end(); it != end; ++it) {
        if (it->second > index)
            return it;
    }
    return m_ranges.end();
}

void IndexSet::add(size_t index)
{
    do_add(find(index), index);
}

size_t IndexSet::add_shifted(size_t index)
{
    auto it = m_ranges.begin();
    for (auto end = m_ranges.end(); it != end && it->first <= index; ++it) {
        index += it->second - it->first;
    }
    do_add(it, index);
    return index;
}

void IndexSet::set(size_t len)
{
    m_ranges.clear();
    if (len) {
        m_ranges.push_back({0, len});
    }
}

void IndexSet::insert_at(size_t index)
{
    auto pos = find(index);
    if (pos != m_ranges.end()) {
        if (pos->first >= index)
            ++pos->first;
        ++pos->second;
        for (auto it = pos + 1; it != m_ranges.end(); ++it) {
            ++it->first;
            ++it->second;
        }
    }
    do_add(pos, index);
}

void IndexSet::shift_for_insert_at(size_t index)
{
    auto it = find(index);
    if (it == m_ranges.end())
        return;

    if (it->first < index) {
        // split the range so that we can exclude `index`
        auto old_second = it->second;
        it->second = index;
        it = m_ranges.insert(it + 1, {index, old_second});
    }

    for (; it != m_ranges.end(); ++it) {
        ++it->first;
        ++it->second;
    }
}

bool IndexSet::erase_at(size_t index)
{
    auto it = find(index);
    if (it == m_ranges.end())
        return false;

    bool ret = false;
    if (it->first <= index) {
        ret = true;
        --it->second;
        if (it->first == it->second) {
            it = m_ranges.erase(it);
        }
        else {
            ++it;
        }
    }
    else if (it != m_ranges.begin() && (it - 1)->second + 1 == it->first) {
        (it - 1)->second = it->second - 1;
        it = m_ranges.erase(it);
    }

    for (; it != m_ranges.end(); ++it) {
        --it->first;
        --it->second;
    }
    return ret;
}

size_t IndexSet::unshift(size_t index) const
{
    auto shifted = index;
    for (auto range : m_ranges) {
        if (range.first >= index)
            break;
        shifted -= std::min(range.second, index) - range.first;
    }
    return shifted;
}

void IndexSet::clear()
{
    m_ranges.clear();
}

void IndexSet::do_add(iterator it, size_t index)
{
    bool more_before = it != m_ranges.begin(), valid = it != m_ranges.end();
    if (valid && it->first <= index && it->second > index) {
        // index is already in set
    }
    else if (more_before && (it - 1)->second == index) {
        // index is immediately after an existing range
        ++(it - 1)->second;

        if (valid && (it - 1)->second == it->first) {
            // index joins two existing ranges
            (it - 1)->second = it->second;
            m_ranges.erase(it);
        }
    }
    else if (valid && it->first == index + 1) {
        // index is immediately before an existing range
        --it->first;
    }
    else {
        // index is not next to an existing range
        m_ranges.insert(it, {index, index + 1});
    }
}
