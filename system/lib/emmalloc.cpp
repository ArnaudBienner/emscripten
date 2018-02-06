
/*
 * Simple minimalistic but efficient malloc/free.
 *
 * Assumptions:
 *
 *  - 32-bit system.
 *  - Single-threaded.
 *  - sbrk() is used, and nothing else.
 *  - sbrk() will not be accessed by anyone else.
 *
 * Invariants:
 *
 *  - Metadata is 16 bytes, allocation payload is a
 *    multiple of 16 bytes.
 *  - All regions of memory are adjacent.
 *  - Due to the above, after initial alignment fixing, all
 *    regions are aligned.
 *  - A region is either in use (used payload > 0) or not.
 *    Used regions may be adjacent, and a used and unused region
 *    may be adjacent, but not two unused ones - they would be
 *    merged.
 *
 * Debugging:
 *
 *  - If not NDEBUG, runtime assert()s are in use.
 *  - If EMMALLOC_DEBUG is defined, a large amount of extra checks are done.
 *  - If EMMALLOC_DEBUG_LOG is defined, a lot of operations are logged
 *    out, in addition to EMMALLOC_DEBUG.
 *  - Debugging and logging uses EM_ASM, not printf etc., to minimize any
 *    risk of debugging or logging depending on malloc.
 */

#define EMMALLOC_DEBUG
//#define EMMALLOC_DEBUG_LOG

#include <assert.h>
#include <string.h> // for memcpy, memset
#include <unistd.h> // for sbrk()

// Debugging

#ifdef EMMALLOC_DEBUG_LOG
#include <emscripten.h>
#define EMMALLOC_DEBUG
#endif

#ifdef EMMALLOC_DEBUG
// Forward declaration for convenience.
void emmalloc_validate_all();
#endif

// Math utilities

static bool isPowerOf2(size_t x) {
  return __builtin_popcount(x) == 1;
}

static size_t lowerBoundPowerOf2(size_t x) {
  if (x == 0) return 1;
  // e.g. 5 is 0..0101, so clz is 29, and we want
  // 4 which is 1 << 2, so the result should be 2
  return 31 - __builtin_clz(x);
}

// Constants

// All allocations are aligned to this value.
static const size_t ALIGNMENT = 16;

// Even allocating 1 byte incurs this much actual payload
// allocation. This is our minimum bin size.
static const size_t ALLOC_UNIT = ALIGNMENT;

// How big the metadata is in each region. It is convenient
// that this is identical to the above values.
static const size_t METADATA_SIZE = ALLOC_UNIT;

// How big a minimal region is.
static const size_t MIN_REGION_SIZE = METADATA_SIZE + ALLOC_UNIT;

// Constant utilities

// Align a pointer, increasing it upwards as necessary
static size_t alignUp(size_t ptr) {
  return (size_t(ptr) + ALIGNMENT - 1) & -ALIGNMENT;
}

static void* alignUpPointer(void* ptr) {
  return (void*)alignUp(size_t(ptr));
}

//
// Data structures
//

struct Region;

// Information memory that is a free list, i.e., may
// be reused.
struct FreeInfo {
  // free lists are doubly-linked lists
  FreeInfo* prev;
  FreeInfo* next;
};

// A contiguous region of memory. Metadata at the beginning describes it,
// after which is the "payload", the sections that user code calling
// malloc can use.
struct Region {
  // The total size of the section of memory this is associated
  // with and contained in.
  // That includes the metadata itself and the payload memory after,
  // which includes the used and unused portions of it.
  size_t totalSize;

  // How many bytes are used out of the payload. If this is 0, the
  // region is free for use (we don't allocate payloads of size 0).
  size_t usedPayload;

  // Each memory area knows its neighbors, as we hope to merge them.
  // If there is no neighbor, NULL.
  Region* prev;
  Region* next;

  // Up to here was the fixed metadata, of size 16. The rest is either
  // the payload, or freelist info.
  union {
    char payload[];
    FreeInfo freeInfo;
  };
};

// Region utilities

static void initRegion(Region* region, size_t totalSize, size_t usedPayload) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.initRegion " + [$0, $1, $2]) }, region, totalSize, usedPayload);
#endif
  region->totalSize = totalSize;
  region->usedPayload = usedPayload;
  region->prev = NULL;
  region->next = NULL;
}

static void* getPayload(Region* region) {
  assert(((char*)&region->freeInfo) - ((char*)region) == METADATA_SIZE);
  assert(region->usedPayload);
  return &region->payload;
}

static Region* fromPayload(void* payload) {
  return (Region*)((char*)payload - METADATA_SIZE);
}

static Region* fromFreeInfo(FreeInfo* freeInfo) {
  return (Region*)((char*)freeInfo - METADATA_SIZE);
}

static size_t getMaxPayload(Region* region) {
  return region->totalSize - METADATA_SIZE;
}

static void* getAfter(Region* region) {
  return ((char*)region) + region->totalSize;
}

// Globals

// TODO: For now we have a single global space for all allocations,
//       but for multithreading etc. we may want to generalize that.

// A freelist (a list of Regions ready for re-use) for all
// power of 2 payload sizes (only the ones from ALIGNMENT
// size and above are relevant, though). The freelist at index
// K contains regions of memory big enough to contain at least
// 2^K bytes.
//
// Note that there is no freelist for 2^32, as that amount can
// never be allocated.

static const size_t MIN_FREELIST_INDEX = 4;  // 16 == ALLOC_UNIT
static const size_t MAX_FREELIST_INDEX = 32; // uint32_t

static FreeInfo* freeLists[MAX_FREELIST_INDEX] = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

// The first region of memory. This is not actually needed unless
// we are debugging, in which case it is the start of the linked list
// of all the regions.
#ifdef EMMALLOC_DEBUG
static Region* firstRegion = NULL;
#endif

// The last region of memory. It's important to know the end
// since we may append to it.
static Region* lastRegion = NULL;

// Global utilities

// The freelist index is where we would appear in a freelist if
// we were one. It is a list of items of size at least the power
// of 2 that lower bounds us.
static size_t getFreeListIndex(size_t size) {
  assert(1 << MIN_FREELIST_INDEX == ALLOC_UNIT);
  assert(size > 0);
  if (size < ALLOC_UNIT) size = ALLOC_UNIT;
  // We need a lower bound here, as the list contains things
  // that can contain at least a power of 2.
  size_t index = lowerBoundPowerOf2(size);
  assert(MIN_FREELIST_INDEX <= index && index < MAX_FREELIST_INDEX);
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.getFreeListIndex " + [$0, $1]) }, size, index);
#endif
  return index;
}

// The big-enough freelist index is the index of the freelist of
// items that are all big enough for us. This is computed using
// an upper bound power of 2.
static size_t getBigEnoughFreeListIndex(size_t size) {
  size_t index = getFreeListIndex(size);
  // If we're a power of 2, the lower and upper bounds are the
  // same. Otherwise, add one.
  if (!isPowerOf2(size)) index++;
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.getBigEnoughFreeListIndex " + [$0, $1]) }, size, index);
#endif
  return index;
}

// Items in the freelist at this index must be at least this large.
static size_t getMinSizeForFreeListIndex(size_t index) {
  return 1 << index;
}

// Items in the freelist at this index must be smaller than this.
static size_t getMaxSizeForFreeListIndex(size_t index) {
  return 1 << (index + 1);
}

static void removeFromFreeList(Region* region) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.removeFromFreeList " + $0) },region);
#endif
  assert(!region->usedPayload);
  size_t index = getFreeListIndex(getMaxPayload(region));
  FreeInfo* freeInfo = &region->freeInfo;
  if (freeLists[index] == freeInfo) {
    freeLists[index] = freeInfo->next;
  }
  if (freeInfo->prev) {
    freeInfo->prev->next = freeInfo->next;
  }
  if (freeInfo->next) {
    freeInfo->next->prev = freeInfo->prev;
  }
}

static void addToFreeList(Region* region) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.addToFreeList " + $0) }, region);
#endif
  assert(!region->usedPayload);
  assert(getAfter(region) <= sbrk(0));
  size_t index = getFreeListIndex(getMaxPayload(region));
  FreeInfo* freeInfo = &region->freeInfo;
  FreeInfo* last = freeLists[index];
  freeLists[index] = freeInfo;
  freeInfo->prev = NULL;
  freeInfo->next = last;
  if (last) {
    last->prev = freeInfo;
  }
}

// Receives a region that has just become free (and is not yet in a freelist).
// Tries to merge it into a region before or after it to which it is adjacent.
int mergeIntoExistingFreeRegion(Region* region) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.mergeIntoExistingFreeRegion " + $0) }, region);
#endif
  assert(getAfter(region) <= sbrk(0));
  int merged = 0;
  Region* prev = region->prev;
  Region* next = region->next;
  if (prev && !prev->usedPayload) {
    // Merge them.
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.mergeIntoExistingFreeRegion merge into prev " + $0) }, prev);
#endif
    removeFromFreeList(prev);
    prev->totalSize += region->totalSize;
    prev->next = region->next;
    if (next) {
      next->prev = prev; // was: region
    } else {
      assert(region == lastRegion);
      lastRegion = prev;
    }
    if (next) {
      // We may also be able to merge with the next, keep trying.
      if (!next->usedPayload) {
#ifdef EMMALLOC_DEBUG_LOG
        EM_ASM({ Module.print("  emmalloc.mergeIntoExistingFreeRegion also merge into next " + $0) }, next);
#endif
        removeFromFreeList(next);
        prev->totalSize += next->totalSize;
        prev->next = next->next;
        if (prev->next) {
          prev->next->prev = prev;
        } else {
          assert(next == lastRegion);
          lastRegion = prev;
        }
      }
    }
    addToFreeList(prev);
    return 1;
  }
  if (next && !next->usedPayload) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.mergeIntoExistingFreeRegion merge into next " + $0) }, next);
#endif
    // Merge them.
    removeFromFreeList(next);
    region->totalSize += next->totalSize;
    region->next = next->next;
    if (region->next) {
      region->next->prev = region;
    } else {
      assert(next == lastRegion);
      lastRegion = region;
    }
    addToFreeList(region);
    return 1;
  }
  return 0;
}

static void possiblySplitRemainder(Region* region, size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.possiblySplitRemainder " + [$0, $1]) }, region, size);
#endif
  size_t payloadSize = getMaxPayload(region);
  assert(payloadSize >= size);
  size_t extra = payloadSize - size;
  // We need room for a minimal region.
  if (extra >= MIN_REGION_SIZE) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("    emmalloc.possiblySplitRemainder is splitting") });
#endif
    // Worth it, split the region
    // TODO: Consider not doing it, may affect long-term fragmentation.
    void* after = getAfter(region);
    Region* split = (Region*)alignUpPointer((char*)getPayload(region) + size);
    region->totalSize = (char*)split - (char*)region;
    size_t totalSplitSize = (char*)after - (char*)split;
    assert(totalSplitSize >= MIN_REGION_SIZE);
    initRegion(split, totalSplitSize, 0);
    split->prev = region;
    split->next = region->next;
    region->next = split;
    if (split->next) {
      split->next->prev = split;
    } else {
      assert(region == lastRegion);
      lastRegion = split;
    }
    // It may be mergable into something adjacent (e.g. if we split off part of
    // a used region during realloc, and there was a free region after it).
    if (!mergeIntoExistingFreeRegion(split)) {
      addToFreeList(split);
    }
  }
}

static void useRegion(Region* region, size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.useRegion " + [$0, $1]) }, region, size);
#endif
  region->usedPayload = size;
  // We may not be using all of it, split out a smaller
  // region into a free list if it's large enough.
  possiblySplitRemainder(region, size);
}

static Region* useFreeInfo(FreeInfo* freeInfo, size_t size) {
  Region* region = fromFreeInfo(freeInfo);
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.useFreeInfo " + [$0, $1]) }, region, size);
#endif
  // This region is no longer free
  removeFromFreeList(region);
  // This region is now in use
  useRegion(region, size);
  return region;
}

// Debugging

#ifdef EMMALLOC_DEBUG
// For testing purposes, wipes everything.
void emmalloc_blank_slate_from_orbit() {
  for (int i = 0; i < MAX_FREELIST_INDEX; i++) {
    freeLists[i] = NULL;
  }
  firstRegion = NULL;
  lastRegion = NULL;
}

// For testing purposes, validate a region.
void emmalloc_validate_region(Region* region) {
  assert(getAfter(region) <= sbrk(0));
  assert(region->usedPayload <= region->totalSize);
  if (region->prev) {
    assert(getAfter(region->prev) == region);
    assert(region->prev->next == region);
  }
  if (region->next) {
    assert(getAfter(region) == region->next);
    assert(region->next->prev == region);
  }
}

// For testing purposes, check that everything is valid.
void emmalloc_validate_all() {
  void* end = sbrk(0);
  // Validate regions.
  Region* curr = firstRegion;
  Region* prev = NULL;
  while (curr) {
    assert(curr->prev == prev);
    if (prev) {
      assert(getAfter(prev) == curr);
      assert(prev->next = curr);
      // Adjacent free regions must be merged.
      assert(!(!prev->usedPayload && !curr->usedPayload));
    }
    assert(getAfter(curr) <= end);
    prev = curr;
    curr = curr->next;
  }
  if (prev) {
    assert(prev == lastRegion);
  } else {
    assert(!lastRegion);
  }
  // Validate freelists.
  for (int i = 0; i < MAX_FREELIST_INDEX; i++) {
    FreeInfo* curr = freeLists[i];
    if (!curr) continue;
    FreeInfo* prev = NULL;
    while (curr) {
      assert(curr->prev == prev);
      Region* region = fromFreeInfo(curr);
      assert(getAfter(region) <= end);
      assert(getMaxPayload(region) >= getMinSizeForFreeListIndex(i));
      assert(getMaxPayload(region) <  getMaxSizeForFreeListIndex(i));
      prev = curr;
      curr = curr->next;
    }
  }
  // Validate lastRegion.
  if (lastRegion) {
    assert(lastRegion->next == NULL);
    assert(getAfter(lastRegion) <= end);
    assert(firstRegion);
  } else {
    assert(!firstRegion);
  }
}

#ifdef EMMALLOC_DEBUG_LOG
// For testing purposes, dump out a region.
void emmalloc_dump_region(Region* region) {
  EM_ASM({ Module.print("      [" + $0 + " - " + $1 + " (used: " + $2 + " / " + $3 + ")] prev: " + $4 + " next: " + $5) },
         region, getAfter(region), region->usedPayload, getMaxPayload(region), region->prev, region->next);
}

// For testing purposes, dumps out the entire global state.
void emmalloc_dump_all() {
  EM_ASM({ Module.print("  emmalloc_dump_everything:\n    sbrk(0) = " + $0) }, sbrk(0));
  Region* curr = firstRegion;
  EM_ASM({ Module.print("    all regions:") });
  while (curr) {
    emmalloc_dump_region(curr);
    curr = curr->next;
  }
  for (int i = 0; i < MAX_FREELIST_INDEX; i++) {
    FreeInfo* curr = freeLists[i];
    if (!curr) continue;
    EM_ASM({ Module.print("    freeList[" + $0 + "] sizes: [" + $1 + ", " + $2 + ")") }, i, getMinSizeForFreeListIndex(i), getMaxSizeForFreeListIndex(i));
    FreeInfo* prev = NULL;
    while (curr) {
      Region* region = fromFreeInfo(curr);
      emmalloc_dump_region(region);
      prev = curr;
      curr = curr->next;
    }
  }
}
#endif // EMMALLOC_DEBUG_LOG
#endif // EMMALLOC_DEBUG

// When we free something of size 100, we put it in the
// freelist for items of size 64 and above. Then when something
// needs 64 bytes, we know the things in that list are all
// suitable. However, note that this means that if we then
// try to allocate something of size 100 once more, we will
// look in the freelist for items of size 128 or more (again,
// so we know all items in the list are big enough), which means
// we may not reuse the perfect region we just freed. It's hard
// to do a perfect job on that without a lot more work (memory
// and/or time), so instead, we use a simple heuristic to look
// at the one-lower freelist, which *may* contain something
// big enough for us. We look at just a few elements, but that is
// enough if we are alloating/freeing a lot of such elements
// (since the recent items are there).
// TODO: Consider more optimizations, e.g. slow bubbling of larger
//       items in each freelist towards the root, or even actually
//       keep it sorted by size.
// Consider also what happens to the very largest allocations,
// 2^32 - a little. That goes in the freelist of items of size
// 2^31 or less. 3 tries is enough to go through that entire
// freelist because even 2 can't exist, they'd exhaust memory
// (together with metadata overhead). So we should be able to
// free and allocate such largest allocations (barring fragmentation
// happening in between).
static const size_t SPECULATIVE_FREELIST_TRIES = 3;

static Region* tryFromFreeList(size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.tryFromFreeList " + $0) }, size);
#endif
  // Look in the freelist of items big enough for us.
  size_t index = getBigEnoughFreeListIndex(size);
  // If we *may* find an item in the index one
  // below us, try that briefly in constant time;
  // see comment on algorithm on the declaration of
  // SPECULATIVE_FREELIST_TRIES.
  if (index > MIN_FREELIST_INDEX &&
      size < getMinSizeForFreeListIndex(index)) {
    FreeInfo* freeInfo = freeLists[index - 1];
    size_t tries = 0;
    while (freeInfo && tries < SPECULATIVE_FREELIST_TRIES) {
      Region* region = fromFreeInfo(freeInfo);
      if (getMaxPayload(region) >= size) {
        // Success, use it
#ifdef EMMALLOC_DEBUG_LOG
        EM_ASM({ Module.print("    emmalloc.tryFromFreeList try succeeded") });
#endif
        return useFreeInfo(freeInfo, size);
      }
      freeInfo = freeInfo->next;
      tries++;
    }
  }
  // Note that index may start out at MAX_FREELIST_INDEX,
  // if it is almost the largest allocation possible,
  // 2^32 minus a little. In that case, looking in the lower
  // freelist is our only hope, and it can contain at most 1
  // element (see discussion above), so we will find it if
  // it's there). If not, and we got here, we'll never enter
  // the loop at all.
  while (index < MAX_FREELIST_INDEX) {
    FreeInfo* freeInfo = freeLists[index];
    if (freeInfo) {
      // We found one, use it.
#ifdef EMMALLOC_DEBUG_LOG
      EM_ASM({ Module.print("    emmalloc.tryFromFreeList had item to use") });
#endif
      return useFreeInfo(freeInfo, size);
    }
    // Look in a freelist of larger elements.
    // TODO This does increase the risk of fragmentation, though,
    //      and maybe the iteration adds runtime overhead.
    index++;
  }
  // No luck, no free list.
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("    emmalloc.tryFromFreeList no luck") });
#endif
  return NULL;
}

// Extends the last region to a certain size. Returns 0 if successful,
// 1 if an error occurred in sbrk().
static int extendLastRegion(size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("    emmalloc.extendLastRegionToSize " + $0) }, size);
#endif
  assert(size > lastRegion->usedPayload);
  assert(size > getMaxPayload(lastRegion));
  size_t reusable = getMaxPayload(lastRegion);
  // We should only do a new allocation when we must, so the last region
  // was not sufficient.
  assert(reusable < size);
  size_t sbrkSize = alignUp(size) - reusable;
  void* ptr = sbrk(sbrkSize);
  if (ptr == (void*)-1) {
    // sbrk() failed, we failed.
#ifdef EMMALLOC_DEBUG_LOG
   EM_ASM({ Module.print("    emmalloc.extendLastRegion sbrk failure") });
#endif
    return 1;
  }
  // sbrk() should give us new space right after the last region.
  assert(ptr == getAfter(lastRegion));
  lastRegion->totalSize += sbrkSize;
  lastRegion->usedPayload = size;
  return 0;
}

static Region* newAllocation(size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("    emmalloc.newAllocation " + $0) }, size);
#endif
  assert(size > 0);
  if (lastRegion) {
    // If the last region is free, we can extend it rather than leave it
    // as fragmented free spce between allocated regions. This is also
    // more efficient and simple as well.
    if (!lastRegion->usedPayload) {
#ifdef EMMALLOC_DEBUG_LOG
     EM_ASM({ Module.print("    emmalloc.newAllocation extending lastRegion at " + $0) }, lastRegion);
#endif
      // Remove it first, before we adjust the size (which affects which list
      // it should be in).
      removeFromFreeList(lastRegion);
      if (extendLastRegion(size) == 0) {
        return lastRegion;
      } else {
        return NULL;
      }
    } else {
      // The last region is not free. But if it has useful free space at the
      // end, we can split that part off and use it
      size_t alignedUsed = alignUp(lastRegion->usedPayload);
      size_t usable = getMaxPayload(lastRegion) - alignedUsed;
      if (usable > 0) {
        assert(usable >= ALLOC_UNIT);
#ifdef EMMALLOC_DEBUG_LOG
        EM_ASM({ Module.print("    emmalloc.newAllocation splitting lastRegion at " + $0) }, lastRegion);
#endif
        size_t sbrkSize = METADATA_SIZE + alignUp(size) - usable;
        void* ptr = sbrk(sbrkSize);
        if (ptr == (void*)-1) {
          // sbrk() failed, we failed.
#ifdef EMMALLOC_DEBUG_LOG
          EM_ASM({ Module.print("    emmalloc.newAllocation sbrk failure") });
#endif
          return NULL;
        }
        // sbrk() should give us new space right after the last region.
        assert(ptr == getAfter(lastRegion));
        Region* region = (Region*)((char*)ptr - usable);
        lastRegion->totalSize -= usable;
        initRegion(region, sbrkSize + usable, size);
        lastRegion->next = region;
        region->prev = lastRegion;
        lastRegion = region;
        return lastRegion;
      }
    }
  }
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("    emmalloc.newAllocation getting brand new space") });
#endif
  size_t sbrkSize = METADATA_SIZE + alignUp(size);
  void* ptr = sbrk(sbrkSize);
  if (ptr == (void*)-1) {
    // sbrk() failed, we failed.
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("    emmalloc.newAllocation sbrk failure") });
#endif
    return NULL;
  }
  // sbrk() results might not be aligned. We assume single-threaded sbrk()
  // access here in order to fix that up
  void* fixedPtr = alignUpPointer(ptr);
  if (ptr != fixedPtr) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("    emmalloc.newAllocation fixing alignment") });
#endif
    size_t extra = (char*)fixedPtr - (char*)ptr;
    void* extraPtr = sbrk(extra);
    if (extraPtr == (void*)-1) {
      // sbrk() failed, we failed.
#ifdef EMMALLOC_DEBUG_LOG
      EM_ASM({ Module.print("    emmalloc.newAllocation sbrk failure") });;
#endif
      return NULL;
    }
    // Verify the sbrk() assumption, no one else should call it.
    // If this fails, it means we also leak the previous allocation,
    // so we don't even try to handle it.
    assert((char*)extraPtr == (char*)ptr + sbrkSize);
    // After the first allocation, everything must remain aligned forever.
    assert(!lastRegion);
    // We now have a contiguous block of memory from ptr to
    // ptr + sbrkSize + fixedPtr - ptr = fixedPtr + sbrkSize.
    // fixedPtr is aligned and starts a region of the right
    // amount of memory.
  }
  Region* region = (Region*)fixedPtr;
  // Success, we have new memory
  initRegion(region, sbrkSize, size);
  useRegion(region, size);
  // Apply globally, connect it to lastRegion
  if (lastRegion) {
    // No one else should be using sbrk(), so we must be adjacent
    assert(region == getAfter(lastRegion));
    assert(lastRegion->next == NULL);
    lastRegion->next = region;
    region->prev = lastRegion;
  }
#ifdef EMMALLOC_DEBUG
  if (!firstRegion) {
    firstRegion = region;
  }
#endif
  lastRegion = region;
  return region;
}

// Internal mirror of public API.

void* emmalloc_malloc(size_t size) {
  // malloc() spec defines malloc(0) => NULL.
  if (size == 0) return NULL;
  // Look in the freelist first.
  Region* region = tryFromFreeList(size);
  if (!region) {
    // Allocate some new memory otherwise.
    region = newAllocation(size);
    if (!region) {
      // We failed to allocate, sadly.
      return NULL;
    }
  }
  assert(getAfter(region) <= sbrk(0));
  return getPayload(region);
}

void emmalloc_free(void *ptr) {
  if (ptr == NULL) return;
  Region* region = fromPayload(ptr);
  region->usedPayload = 0;
  // Perhaps we can join this to an adjacent free region, unfragmenting?
  if (!mergeIntoExistingFreeRegion(region)) {
    // Otherwise, mark as unused and add to freelist.
    addToFreeList(region);
  }
}

void* emmalloc_calloc(size_t nmemb, size_t size) {
  // TODO If we know no one else is using sbrk(), we can assume that new
  //      memory allocations are zero'd out.
  void* ptr = emmalloc_malloc(nmemb * size);
  if (!ptr) return NULL;
  memset(ptr, 0, nmemb * size);
  return ptr;
}

void* emmalloc_realloc(void *ptr, size_t size) {
  if (!ptr) return emmalloc_malloc(size);
  if (!size) {
    emmalloc_free(ptr);
    return NULL;
  }
  Region* region = fromPayload(ptr);
  // Grow it. First, maybe we can do simple growth in the current region.
  if (size <= getMaxPayload(region)) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.emmalloc_realloc use existing payload space") });
#endif
    region->usedPayload = size;
    // There might be enough left over to split out now.
    possiblySplitRemainder(region, size);
    return ptr;
  }
  // Perhaps right after us is free space we can merge to us.
  Region* next = region->next;
  if (next && !next->usedPayload) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.emmalloc_realloc merge in next") });
#endif
    removeFromFreeList(next);
    region->totalSize += next->totalSize;
    region->next = next->next;
    if (region->next) {
      region->next->prev = region;
    } else {
      lastRegion = region;
    }
  }
  // We may now be big enough.
  if (size <= getMaxPayload(region)) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.emmalloc_realloc use existing payload space after merge") });
#endif
    region->usedPayload = size;
    // There might be enough left over to split out now.
    possiblySplitRemainder(region, size);
    return ptr;
  }
  // We still aren't big enough. But if we are the last, extend ourselves.
  if (region == lastRegion) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.emmalloc_realloc extend last region") });
#endif
    if (extendLastRegion(size) == 0) {
      return ptr;
    } else {
      // If this failed, we can also try the normal
      // malloc path, which may find space in a freelist;
      // fall through.
    }
  }
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.emmalloc_realloc slow path: new allocation, copy, free") });
#endif
  void* newPtr = emmalloc_malloc(size);
  if (!newPtr) return NULL;
  memcpy(newPtr, getPayload(region), region->usedPayload);
  emmalloc_free(ptr);
  return newPtr;
}

// Public API. This is a thin wrapper around our mirror of it, adding
// logging and validation when debugging. Otherwise it should inline
// out.

extern "C" {

void* malloc(size_t size) {
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.malloc " + $0) }, size);
#endif
  emmalloc_validate_all();
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
#endif
  void* ptr = emmalloc_malloc(size);
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.malloc ==> " + $0) }, ptr);
#endif
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
  emmalloc_validate_all();
#endif
  return ptr;
}

void free(void *ptr) {
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.free " + $0) }, ptr);
#endif
  emmalloc_validate_all();
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
#endif
  emmalloc_free(ptr);
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
  emmalloc_validate_all();
#endif
}

void* calloc(size_t nmemb, size_t size) {
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.calloc " + $0) }, size);
#endif
  emmalloc_validate_all();
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
#endif
  void* ptr = emmalloc_calloc(nmemb, size);
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.calloc ==> " + $0) }, ptr);
#endif
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
  emmalloc_validate_all();
#endif
  return ptr;
}

void* realloc(void *ptr, size_t size) {
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.realloc " + [$0, $1]) }, ptr, size);
#endif
  emmalloc_validate_all();
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
#endif
  void* newPtr = emmalloc_realloc(ptr, size);
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.realloc ==> " + $0) }, newPtr);
#endif
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
  emmalloc_validate_all();
#endif
  return newPtr;
}

} // extern "C"
