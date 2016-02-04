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

#ifndef REALM_INDEX_SET_HPP
#define REALM_INDEX_SET_HPP

#include <vector>

namespace realm {
class IndexSet {
public:
    using value_type = std::pair<size_t, size_t>;
    using iterator = std::vector<value_type>::iterator;
    using const_iterator = std::vector<value_type>::const_iterator;

    const_iterator begin() const { return m_ranges.begin(); }
    const_iterator end() const { return m_ranges.end(); }
    bool empty() const { return m_ranges.empty(); }
    size_t size() const { return m_ranges.size(); }

    // Check if the index set contains the given index
    bool contains(size_t index) const;

    // Add an index to the set, doing nothing if it's already present
    void add(size_t index);

    // Add an index which has had all of the ranges in the set before it removed
    // Returns the unshifted index
    size_t add_shifted(size_t index);

    // Remove all indexes from the set and then add a single range starting from
    // zero with the given length
    void set(size_t len);

    // Insert an index at the given position, shifting existing indexes at or
    // after that point back by one
    void insert_at(size_t index);

    // Shift indexes at or after the given point back by one
    void shift_for_insert_at(size_t index);

    // Delete an index at the given position, shifting indexes after that point
    // forward by one
    // returns whether the index was contained in the set
    bool erase_at(size_t index);

    // this is probably wrong
    size_t unshift(size_t index) const;

    void clear();

    class IndexInterator {
    public:
        IndexInterator(IndexSet::const_iterator it) : m_iterator(it) { }
        size_t operator*() const { return m_iterator->first + m_offset; }
        bool operator!=(IndexInterator const& it) const { return m_iterator != it.m_iterator; }

        IndexInterator& operator++()
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

    class IndexIteratableAdaptor {
    public:
        using value_type = size_t;
        using iterator = IndexInterator;
        using const_iterator = iterator;

        const_iterator begin() const { return m_index_set.begin(); }
        const_iterator end() const { return m_index_set.end(); }

        IndexIteratableAdaptor(IndexSet const& is) : m_index_set(is) { }
    private:
        IndexSet const& m_index_set;
    };

    IndexIteratableAdaptor as_indexes() const { return *this; }

private:
    std::vector<value_type> m_ranges;

    // Find the range which contains the index, or the first one after it if
    // none do
    iterator find(size_t index);
    // Insert the index before the given position, combining existing ranges as
    // applicable
    void do_add(iterator pos, size_t index);
};
} // namespace realm

#endif // REALM_INDEX_SET_HPP
