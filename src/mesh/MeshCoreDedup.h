#pragma once

#include <stddef.h>
#include <stdint.h>

/** Fixed-size ring of recently seen MeshCore frame hashes. 0 marks an empty slot and is never stored. */
template <size_t N> class MeshCoreDedup
{
  public:
    bool contains(uint32_t hash) const
    {
        for (size_t i = 0; i < N; i++) {
            if (entries[i] == hash)
                return true;
        }
        return false;
    }

    void remember(uint32_t hash)
    {
        entries[next] = hash;
        next = (next + 1) % N;
    }

  private:
    uint32_t entries[N] = {};
    size_t next = 0;
};
