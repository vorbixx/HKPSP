#pragma once

#include <cstddef>
#include <cstdint>

namespace hk3ds
{
// Bounded, name-addressed values persisted with a tutorial save. Zero values
// are represented by absence, so the table remains compact and canonical.
struct SavedPlayerData
{
    static constexpr std::size_t capacity = 64;

    struct Entry
    {
        std::uint32_t key = 0;
        std::int32_t value = 0;
    };

    Entry entries[capacity]{};

    static std::uint32_t key(const char* name)
    {
        if (!name) return 0;
        std::uint32_t hash = 2166136261u;
        while (*name)
        {
            hash ^= static_cast<std::uint8_t>(*name++);
            hash *= 16777619u;
        }
        return hash;
    }

    std::int32_t get(std::uint32_t id) const
    {
        if (id == 0) return 0;
        for (const Entry& entry : entries)
        {
            if (entry.key == id) return entry.value;
            if (entry.key == 0 || entry.key > id) break;
        }
        return 0;
    }

    std::int32_t get(const char* name) const { return get(key(name)); }

    bool set(std::uint32_t id, std::int32_t value)
    {
        if (id == 0 || !valid()) return false;
        std::size_t index = 0;
        while (index < capacity && entries[index].key != 0 && entries[index].key < id) ++index;
        if (index < capacity && entries[index].key == id)
        {
            if (value != 0)
            {
                entries[index].value = value;
                return true;
            }
            for (std::size_t move = index; move + 1 < capacity; ++move)
                entries[move] = entries[move + 1];
            entries[capacity - 1] = {};
            return true;
        }
        if (value == 0) return true;
        if (index == capacity || entries[capacity - 1].key != 0) return false;
        for (std::size_t move = capacity - 1; move > index; --move)
            entries[move] = entries[move - 1];
        entries[index] = {id, value};
        return true;
    }

    bool set(const char* name, std::int32_t value) { return set(key(name), value); }

    bool valid() const
    {
        std::uint32_t previous = 0;
        bool emptySeen = false;
        for (const Entry& entry : entries)
        {
            if (entry.key == 0)
            {
                if (entry.value != 0) return false;
                emptySeen = true;
                continue;
            }
            if (emptySeen || entry.key <= previous || entry.value == 0) return false;
            previous = entry.key;
        }
        return true;
    }

    bool operator==(const SavedPlayerData& other) const
    {
        for (std::size_t i = 0; i < capacity; ++i)
            if (entries[i].key != other.entries[i].key || entries[i].value != other.entries[i].value)
                return false;
        return true;
    }

    bool operator!=(const SavedPlayerData& other) const { return !(*this == other); }
};
}
