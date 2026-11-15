#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addArenaDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // arena
      {"arena_new", {"arena_new", "arena_new(capacity) -> Allocator",
                     "Bump allocator: allocations are fast, release is all-or-nothing."}},
      {"arena_pool", {"arena_pool", "arena_pool(capacity) -> Allocator",
                      "Pool allocator: released blocks are reused for equal sizes."}},
      {"arena_region", {"arena_region", "arena_region(capacity) -> Allocator",
                        "Region allocator: releases must happen in reverse allocation order."}},
      {"arena_alloc", {"arena_alloc", "arena_alloc(allocator, size) -> i64",
                        "Allocates bytes and returns a handle; errors when out of space."}},
      {"arena_release", {"arena_release", "arena_release(allocator, handle)",
                          "Returns an allocation to the allocator."}},
      {"arena_reset", {"arena_reset", "arena_reset(allocator)",
                        "Frees every allocation the allocator owns."}},
      {"arena_used", {"arena_used", "arena_used(allocator) -> i64",
                      "Bytes currently handed out."}},
      {"arena_total", {"arena_total", "arena_total(allocator) -> i64",
                        "Bytes of capacity."}},
  });
}

}  // namespace a
