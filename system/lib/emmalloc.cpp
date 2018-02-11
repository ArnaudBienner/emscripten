/*
 * Simple minimalistic but efficient malloc/free.
 *
 * Assumptions:
 *
 *  - 32-bit system.
 *  - Single-threaded.
 *  - sbrk() is used, and nothing else.
 *  - sbrk() will not be accessed by anyone else.
 *  - sbrk() is very fast in most cases (internal wasm call).
 *
 * Invariants and design:
 *
 *  - Default alignment for memory is 8 bytes.
 *  - Payload allocation is done in units of 4 bytes.
 *  - The minimal payload is 8 bytes. This allows embedding the freelist
 *    pointers when a region is not in use. While this does mean that
 *    4-byte allocations are less efficient, in practice such allocations
 *    are quite rare.
 *  - Metadata is 4 or 8 bytes. The "mini" 4 byte version is used
 *    when the region and its predecessor are small enough (so that
 *    we can fit all the info in 32 bits), and when doing so would
 *    help us avoid wasting space due to alignment. For example,
 *    if we are at an aligned address - a multiple of 8 - then
 *    we may as well use the regular 8 byte metadata, as then the
 *    payload will be aligned. But if we not 8 byte aligned, and only
 *    4, then we'll try to use a mini metadata.
 *     - If the region or the previous one are quite large, then
 *       wasting 4 bytes or so doesn't matter anyhow. But for many
 *       small allocations this is significant.
 *     - The one tricky thing here is that we also need the previous
 *       region to be small enough. If it grows, we need to make sure
 *       it doesn't grow too much, which can be a source of
 *       fragmentation, but as it is limited by the number of quite
 *       large regions, it tends to be minimal.
 *  - All regions of memory are adjacent.
 *  - A region is either in use (used payload > 0) or not.
 *    Used regions may be adjacent, and a used and unused region
 *    may be adjacent, but not two unused ones - they would be
 *    merged, *except* for the case where we can't merge free space
 *    due to the later region having a mini header.
 *  - A used region always has minimal space at the end - we
 *    split off extra space when possible immediately. This lets us
 *    just use 1 bit to indicate if the region is used or not. This
 *    does mean we may memcpy() more than needed by a little when
 *    doing a realloc, though.
 *
 * Debugging:
 *
 *  - If not NDEBUG, runtime assert()s are in use.
 *  - If EMMALLOC_DEBUG is defined, a large amount of extra checks are done.
 *  - If EMMALLOC_DEBUG_LOG is defined, a lot of operations are logged
 *    out, in addition to EMMALLOC_DEBUG.
 *  - Debugging and logging uses EM_ASM, not printf etc., to minimize any
 *    risk of debugging or logging depending on malloc.
 *
 * TODO
 *
 *  - Optimizations for small allocations that are not multiples of 8, like
 *    12 and 20 (which take 24 and 32 bytes respectively)
 *
 */

#include <assert.h>
#include <malloc.h> // mallinfo
#include <string.h> // for memcpy, memset
#include <unistd.h> // for sbrk()
#include <emscripten.h>

#define EMMALLOC_EXPORT __attribute__((__weak__, __visibility__("default")))

// Debugging

#ifdef EMMALLOC_DEBUG_LOG
#ifndef EMMALLOC_DEBUG
#define EMMALLOC_DEBUG
#endif
#endif

#ifdef EMMALLOC_DEBUG
// Forward declaration for convenience.
static void emmalloc_validate_all();
#endif
#ifdef EMMALLOC_DEBUG
// Forward declaration for convenience.
static void emmalloc_dump_all();
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
static const size_t ALIGNMENT = 8;

// Allocation of payloads is at least of this size.
static const size_t MIN_ALLOC = 8;

// Allocation is done in multiples of this amount.
static const size_t ALLOC_UNIT = 4;

// How big the metadata is in each region.
static const size_t MINI_METADATA_SIZE = 4;
static const size_t NORMAL_METADATA_SIZE = 8;

// How big a minimal normal region is.
static const size_t MIN_NORMAL_REGION_SIZE = NORMAL_METADATA_SIZE + ALLOC_UNIT;

// How many bits are used to store the size, and how to shift it.
static const size_t NORMAL_SIZE_BITS = 30;
static const size_t NORMAL_SIZE_SHIFTS = 2;

static const size_t MINI_SIZE_BITS = 15;
static const size_t MINI_SIZE_SHIFTS = 2;

// How many bits are used to store the offset to the previous region, and how to shift it.
static const size_t MINI_PREV_BITS = 15;
static const size_t MINI_PREV_SHIFTS = 2;

// General utilities

// Align a pointer, increasing it upwards as necessary
static size_t alignUpPointer(size_t ptr) {
  return (size_t(ptr) + ALIGNMENT - 1) & -ALIGNMENT;
}

static void* alignUpPointer(void* ptr) {
  return (void*)alignUpPointer(size_t(ptr));
}

static void getAllocSize(size_t size) {
  if (size < MIN_ALLOC) return MIN_ALLOC;
  return (size + ALLOC_UNIT - 1) & -ALLOC_UNIT;
}

//
// Data structures
//

struct Region;

// Information memory that is a free list, i.e., may
// be reused.
// Note how this can fit instead of the payload (as
// the payload is a multiple of MIN_ALLOC).
struct FreeInfo {
  // free lists are doubly-linked lists
  FreeInfo* _prev;
  FreeInfo* _next;

  FreeInfo*& prev() { return _prev; }
  FreeInfo*& next() { return _next; }
};

// The first region of memory.
static Region* firstRegion = NULL;

// The last region of memory.
static Region* lastRegion = NULL;

struct NormalMetadata {
  // This will be false. Note how this is in harmony with mini metadata
  // and the second word of data here, the prev pointer, and as a result
  // the very first bit is how we can test if something is mini
  // or not, and that works on either the single word of a mini,
  // or either of the words of the normal (we may arrive at the first
  // if arriving from the previous, or the second if looking 4 back from
  // the payload).
  size_t _isMini : 1;

  // Whether this region is in use or not.
  size_t _used : 1;

  // The total size of the section of memory this is associated
  // with and contained in.
  // That includes the metadata itself and the payload memory after,
  // which includes the used and unused portions of it.
  // This should be shifted by 2 to get the actual value (note that
  // the allocation is a multiple of 4 anyhow).
  size_t _totalSize : NORMAL_SIZE_BITS;

  // Each memory area knows its previous neighbor, as we hope to merge them.
  // To compute the next neighbor we can use the total size, and to know
  // if a neighbor exists we can compare the region to lastRegion.
  // In a normal region, a full pointer is kept to the previous region.
  // Importantly, since all our regions are on addresses that are multiples
  // of 4, the lowest bit is always clear. That is in harmony with how we
  // check for a normal region in the second 4 bytes as well, so it is
  // ok to check if a region is mini or normal in that way on either of
  // them.
  Region* _prev;
};

struct Mini {
  // This will be true.
  size_t _isMini : 1;

  // Whether this region is in use or not.
  size_t _used : 1;

  // As with normal metadata, this value should be shifted by 2, so the
  // range is up to 128K.
  size_t _totalSize : MINI_SIZE_BITS;

  // We store the offset here to the previous region; we don't have room
  // to store a normal pointer. This value should be shifted by 2, as
  // region sizes are a multiple of that size, so the range here is up
  // to 128K.
  // This offset is from the actual start of the mini metadata, which is
  // here - after _prevData.
  size_t _prev : MINI_PREV_BITS;
} mini;

// A contiguous region of memory.
struct Region {
  // A region has either normal or mini metadata.
  union {
    NormalMetadata normal;
    MiniMetadata mini;
  };

  // After the metadata is the payload, if in use, or freelist info
  // if not.

  // Getters/setters.

  int isNormal() {
    // The mini bit is the same place in both versions, and also identical
    // in the two words of a normal metadata.
    return !normal._isMini;
  }
  int isMini() {
    // The mini bit is the same place in both versions.
    assert(normal._isMini == mini._isMini);
    return normal._isMini;
  }
  size_t getUsed() {
    // The used bit is the same place in both versions.
    assert(normal._isUsed == mini._isUsed);
    return normal._isUsed;
  }
  void setUsed(size_t x) {
    // The used bit is the same place in both versions.
    normal._isUsed = x;
    assert(normal._isUsed == mini._isUsed);
  }
  size_t getTotalSize() {
    if (isNormal()) {
      return normal._totalSize << NORMAL_SIZE_SHIFTS;
    } else {
      return mini._totalSize << MINI_SIZE_SHIFTS;
    }
  }
  void setTotalSize(size_t x) {
    if (isNormal()) {
      normal._totalSize = x >> 2;
    } else {
      assert(x < (1 << (MINI_SIZE_BITS + MINI_SIZE_SHIFTS)));
      mini._totalSize = x >> MINI_SIZE_SHIFTS;
    }
  }
  void incTotalSize(size_t x) {
    if (isNormal()) {
      normal._totalSize += x >> NORMAL_SIZE_SHIFTS;
    } else {
      assert(getTotalSize() + x < (1 << (MINI_SIZE_BITS + MINI_SIZE_SHIFTS)));
      mini._totalSize += x >> MINI_SIZE_SHIFTS;
    }
  }
  void decTotalSize(size_t x) {
    if (isNormal()) {
      normal._totalSize -= x >> NORMAL_SIZE_SHIFTS;
    } else {
      mini._totalSize -= x >> MINI_SIZE_SHIFTS;
    }
  }
  Region* getPrev() {
    if (isNormal()) {
      return normal._prev;
    } else {
      // Calculate the offset from the actual metadata start.
      return (void*)(size_t(this) + 4 - (mini._prev << MINI_PREV_SHIFTS));
    }
  }
  void setPrev(void* x) {
    if (isNormal()) {
      normal._prev = x;
    } else {
      // Calculate the offset from the actual metadata start.
      size_t offset = size_t(this) + 4 - size_t(x);
      assert(offset < (1 << (MINI_PREV_BITS + MINI_PREV_SHIFTS)));
      mini._prev = x >> MINI_PREV_SHIFTS;
    }
  }

  // Utilities.

  Region* getNext() {
    // The next region is computed on the fly.
    if (this != lastRegion) {
      return (Region*)((char*)this + getTotalSize());
    } else {
      return NULL;
    }
  }
  void* getPayload() {
    if (isNormal()) {
      return (void*)((char*)this + sizeof(NormalMetadata));
    } else {
      return (void*)((char*)this + sizeof(MiniMetadata));
    }
  }
  FreeInfo* getFreeInfo() {
    assert(!getUsed());
    return (FreeInfo*)getPayload();
  }

  // Static region getters.

  static Region* getFromPayload(void* payload) {
    // Look 4 behind for what is either a mini metadata or
    // the last 4 bytes of a normal one. In both cases,
    // the same bit tells us what it is.
    Region* region = (char*)payload - 4;
    if (region->isNormal()) {
      // We had the last 4 bytes, look 4 behind to get the
      // actual region.
      region = (char*)region - 4;
      assert(region->isNormal());
    }
    return region;
  }
  static Region* getFromFreeInfo(FreeInfo* freeInfo) {
    // The freelist info is in the same place as the payload.
    return getFromPayload((void*)freeInfo);
  }

  // Other utilities.

  size_t getPayloadSize() {
    if (isNormal()) {
      return getTotalSize() - sizeof(NormalMetadata);
    } else {
      return getTotalSize() - sizeof(MiniMetadata);
    }
  }
};

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

XXX do we need separate freelists for align 4 and 8?

static const size_t MIN_FREELIST_INDEX = 3;  // 8 == ALLOC_UNIT
static const size_t MAX_FREELIST_INDEX = 32; // uint32_t

static FreeInfo* freeLists[MAX_FREELIST_INDEX] = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

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
  assert(size > 0);
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
  size_t index = getFreeListIndex(getMaxPayload(region));
  FreeInfo* freeInfo = &region->freeInfo();
  if (freeLists[index] == freeInfo) {
    freeLists[index] = freeInfo->next();
  }
  if (freeInfo->prev()) {
    freeInfo->prev()->next() = freeInfo->next();
  }
  if (freeInfo->next()) {
    freeInfo->next()->prev() = freeInfo->prev();
  }
}

static void addToFreeList(Region* region) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.addToFreeList " + $0) }, region);
#endif
  assert(getAfter(region) <= sbrk(0));
  size_t index = getFreeListIndex(getMaxPayload(region));
  FreeInfo* freeInfo = &region->freeInfo();
  FreeInfo* last = freeLists[index];
  freeLists[index] = freeInfo;
  freeInfo->prev() = NULL;
  freeInfo->next() = last;
  if (last) {
    last->prev() = freeInfo;
  }
}

// Receives a region that has just become free (and is not yet in a freelist).
// Tries to merge it into a region before or after it to which it is adjacent.
static int mergeIntoExistingFreeRegion(Region* region) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.mergeIntoExistingFreeRegion " + $0) }, region);
#endif
  assert(getAfter(region) <= sbrk(0));
  int merged = 0;
  Region* prev = region->prev();
  Region* next = region->next();
  if (prev && !prev->getUsed()) {
    // Merge them.
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.mergeIntoExistingFreeRegion merge into prev " + $0) }, prev);
#endif
    removeFromFreeList(prev);
    prev->incTotalSize(region->getTotalSize());
    if (next) {
      next->prev() = prev; // was: region
    } else {
      assert(region == lastRegion);
      lastRegion = prev;
    }
    if (next) {
      // We may also be able to merge with the next, keep trying.
      if (!next->getUsed()) {
#ifdef EMMALLOC_DEBUG_LOG
        EM_ASM({ Module.print("  emmalloc.mergeIntoExistingFreeRegion also merge into next " + $0) }, next);
#endif
        removeFromFreeList(next);
        prev->incTotalSize(next->getTotalSize());
        if (next != lastRegion) {
          next->next()->prev() = prev;
        } else {
          lastRegion = prev;
        }
      }
    }
    addToFreeList(prev);
    return 1;
  }
  if (next && !next->getUsed()) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.mergeIntoExistingFreeRegion merge into next " + $0) }, next);
#endif
    // Merge them.
    removeFromFreeList(next);
    region->incTotalSize(next->getTotalSize());
    if (next != lastRegion) {
      next->next()->prev() = region;
    } else {
      lastRegion = region;
    }
    addToFreeList(region);
    return 1;
  }
  return 0;
}

static void stopUsing(Region* region) {
  region->setUsed(0);
  if (!mergeIntoExistingFreeRegion(region)) {
    addToFreeList(region);
  }
}

// Grow a region. If not in use, we may need to be in another
// freelist.
// TODO: We can calculate that, to save some work.
static void growRegion(Region* region, size_t sizeDelta) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.growRegion " + [$0, $1]) }, region, sizeDelta);
#endif
  if (!region->getUsed()) {
    removeFromFreeList(region);
  }
  region->incTotalSize(sizeDelta);
  if (!region->getUsed()) {
    addToFreeList(region);
  }
}

// Extends the last region to a certain payload size. Returns 1 if successful,
// 0 if an error occurred in sbrk().
static int extendLastRegion(size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.extendLastRegionToSize " + $0) }, size);
#endif
  size_t reusable = getMaxPayload(lastRegion);
  size_t sbrkSize = alignUp(size) - reusable;
  void* ptr = sbrk(sbrkSize);
  if (ptr == (void*)-1) {
    // sbrk() failed, we failed.
#ifdef EMMALLOC_DEBUG_LOG
   EM_ASM({ Module.print("  emmalloc.extendLastRegion sbrk failure") });
#endif
    return 0;
  }
  // sbrk() should give us new space right after the last region.
  assert(ptr == getAfter(lastRegion));
  // Increment the region's size.
  growRegion(lastRegion, sbrkSize);
  return 1;
}

static void possiblySplitRemainder(Region* region, size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.possiblySplitRemainder " + [$0, $1]) }, region, size);
#endif
  size_t payloadSize = getMaxPayload(region);
  assert(payloadSize >= size);
  size_t extra = payloadSize - size;
  // Room for a minimal region is definitely worth splitting. Otherwise,
  // if we don't have room for a full region, but we do have an allocation
  // unit's worth, and we are the last region, it's worth allocating some
  // more memory to create a region here. The next allocation can reuse it,
  // which is better than leaving it as unused and unreusable space at the
  // end of this region.
  if (region == lastRegion && extra >= ALLOC_UNIT && extra < MIN_REGION_SIZE) {
    // Yes, this is a small-but-useful amount of memory in the final region,
    // extend it.
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("    emmalloc.possiblySplitRemainder pre-extending") });
#endif
    if (extendLastRegion(payloadSize + ALLOC_UNIT)) {
      // Success.
      extra += ALLOC_UNIT;
      assert(extra >= MIN_REGION_SIZE);
    } else {
      return;
    }
  }
  if (extra >= MIN_REGION_SIZE) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("    emmalloc.possiblySplitRemainder is splitting") });
#endif
    // Worth it, split the region
    // TODO: Consider not doing it, may affect long-term fragmentation.
    void* after = getAfter(region);
    Region* split = (Region*)alignUpPointer((char*)getPayload(region) + size);
    region->setTotalSize((char*)split - (char*)region);
    size_t totalSplitSize = (char*)after - (char*)split;
    assert(totalSplitSize >= MIN_REGION_SIZE);
    split->setTotalSize(totalSplitSize);
    split->prev() = region;
    if (region != lastRegion) {
      split->next()->prev() = split;
    } else {
      lastRegion = split;
    }
    stopUsing(split);
  }
}

// Sets the used payload of a region, and does other necessary work when
// starting to use a region, such as splitting off a remainder if there is
// any.
static void useRegion(Region* region, size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.useRegion " + [$0, $1]) }, region, size);
#endif
  assert(size > 0);
  region->setUsed(1);
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

// Mostly for testing purposes, wipes everything.
EMMALLOC_EXPORT
void emmalloc_blank_slate_from_orbit() {
  for (int i = 0; i < MAX_FREELIST_INDEX; i++) {
    freeLists[i] = NULL;
  }
  firstRegion = NULL;
  lastRegion = NULL;
}

#ifdef EMMALLOC_DEBUG
// For testing purposes, validate a region.
static void emmalloc_validate_region(Region* region) {
  assert(getAfter(region) <= sbrk(0));
  assert(getMaxPayload(region) < region->getTotalSize());
  if (region->prev()) {
    assert(getAfter(region->prev()) == region);
    assert(region->prev()->next() == region);
  }
  if (region->next()) {
    assert(getAfter(region) == region->next());
    assert(region->next()->prev() == region);
  }
}

// For testing purposes, check that everything is valid.
static void emmalloc_validate_all() {
  void* end = sbrk(0);
  // Validate regions.
  Region* curr = firstRegion;
  Region* prev = NULL;
  EM_ASM({
    Module.emmallocDebug = {
      regions: {}
    };
  });
  while (curr) {
    // Note all region, so we can see freelist items are in the main list.
    EM_ASM({
      var region = $0;
      assert(!Module.emmallocDebug.regions[region], "dupe region");
      Module.emmallocDebug.regions[region] = 1;
    }, curr);
    assert(curr->prev() == prev);
    if (prev) {
      assert(getAfter(prev) == curr);
      // Adjacent free regions must be merged.
      assert(!(!prev->getUsed() && !curr->getUsed()));
    }
    assert(getAfter(curr) <= end);
    prev = curr;
    curr = curr->next();
  }
  if (prev) {
    assert(prev == lastRegion);
  } else {
    assert(!lastRegion);
  }
  if (lastRegion) {
    assert(getAfter(lastRegion) == end);
  }
  // Validate freelists.
  for (int i = 0; i < MAX_FREELIST_INDEX; i++) {
    FreeInfo* curr = freeLists[i];
    if (!curr) continue;
    FreeInfo* prev = NULL;
    while (curr) {
      assert(curr->prev() == prev);
      Region* region = fromFreeInfo(curr);
      // Regions must be in the main list.
      EM_ASM({
        var region = $0;
        assert(Module.emmallocDebug.regions[region], "free region not in list");
      }, region);
      assert(getAfter(region) <= end);
      assert(!region->getUsed());
      assert(getMaxPayload(region) >= getMinSizeForFreeListIndex(i));
      assert(getMaxPayload(region) <  getMaxSizeForFreeListIndex(i));
      prev = curr;
      curr = curr->next();
    }
  }
  // Validate lastRegion.
  if (lastRegion) {
    assert(lastRegion->next() == NULL);
    assert(getAfter(lastRegion) <= end);
    assert(firstRegion);
  } else {
    assert(!firstRegion);
  }
}

#ifdef EMMALLOC_DEBUG_LOG
// For testing purposes, dump out a region.
static void emmalloc_dump_region(Region* region) {
  EM_ASM({ Module.print("      [" + $0 + " - " + $1 + " (" + $2 + " bytes" + ($3 ? ", used" : "") + ")]") },
         region, getAfter(region), getMaxPayload(region), region->getUsed());
}

// For testing purposes, dumps out the entire global state.
static void emmalloc_dump_all() {
  EM_ASM({ Module.print("  emmalloc_dump_all:\n    sbrk(0) = " + $0) }, sbrk(0));
  Region* curr = firstRegion;
  EM_ASM({ Module.print("    all regions:") });
  while (curr) {
    emmalloc_dump_region(curr);
    curr = curr->next();
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
      curr = curr->next();
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
// 2^31 or less. >2 tries is enough to go through that entire
// freelist because even 2 can't exist, they'd exhaust memory
// (together with metadata overhead). So we should be able to
// free and allocate such largest allocations (barring fragmentation
// happening in between).
static const size_t SPECULATIVE_FREELIST_TRIES = 32;

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
        EM_ASM({ Module.print("  emmalloc.tryFromFreeList try succeeded") });
#endif
        return useFreeInfo(freeInfo, size);
      }
      freeInfo = freeInfo->next();
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
      EM_ASM({ Module.print("  emmalloc.tryFromFreeList had item to use") });
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
  EM_ASM({ Module.print("  emmalloc.tryFromFreeList no luck") });
#endif
  return NULL;
}

// Allocate a completely new region.
static Region* allocateRegion(size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.allocateRegion") });
#endif
  size_t sbrkSize = METADATA_SIZE + alignUp(size);
  void* ptr = sbrk(sbrkSize);
  if (ptr == (void*)-1) {
    // sbrk() failed, we failed.
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("    emmalloc.allocateRegion sbrk failure") });
#endif
    return NULL;
  }
  // sbrk() results might not be aligned. We assume single-threaded sbrk()
  // access here in order to fix that up
  void* fixedPtr = alignUpPointer(ptr);
  if (ptr != fixedPtr) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("    emmalloc.allocateRegion fixing alignment") });
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
  // Apply globally
  if (!lastRegion) {
    assert(!firstRegion);
    firstRegion = region;
    lastRegion = region;
  } else {
    assert(firstRegion);
    region->prev() = lastRegion;
    lastRegion = region;
  }
  // Success, we have new memory
  region->setTotalSize(sbrkSize);
  useRegion(region, size);
  return region;
}

// Allocate new memory. This may reuse part of the last region, only
// allocating what we need.
static Region* newAllocation(size_t size) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.newAllocation " + $0) }, size);
#endif
  assert(size > 0);
  if (lastRegion) {
    // If the last region is free, we can extend it rather than leave it
    // as fragmented free spce between allocated regions. This is also
    // more efficient and simple as well.
    if (!lastRegion->getUsed()) {
#ifdef EMMALLOC_DEBUG_LOG
      EM_ASM({ Module.print("    emmalloc.newAllocation extending lastRegion at " + $0) }, lastRegion);
#endif
      // Remove it first, before we adjust the size (which affects which list
      // it should be in). Also mark it as used so extending it doesn't do
      // freelist computations; we'll undo that if we fail.
      lastRegion->setUsed(1);
      removeFromFreeList(lastRegion);
      if (extendLastRegion(size)) {
        return lastRegion;
      } else {
        lastRegion->setUsed(0);
        return NULL;
      }
    }
  }
  // Otherwise, get a new region.
  return allocateRegion(size);
}

// Internal mirror of public API.

static void* emmalloc_malloc(size_t size) {
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

static void emmalloc_free(void *ptr) {
  if (ptr == NULL) return;
  stopUsing(fromPayload(ptr));
}

static void* emmalloc_calloc(size_t nmemb, size_t size) {
  // TODO If we know no one else is using sbrk(), we can assume that new
  //      memory allocations are zero'd out.
  void* ptr = emmalloc_malloc(nmemb * size);
  if (!ptr) return NULL;
  memset(ptr, 0, nmemb * size);
  return ptr;
}

static void* emmalloc_realloc(void *ptr, size_t size) {
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
    region->setUsed(1);
    // There might be enough left over to split out now.
    possiblySplitRemainder(region, size);
    return ptr;
  }
  // Perhaps right after us is free space we can merge to us.
  Region* next = region->next();
  if (next && !next->getUsed()) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.emmalloc_realloc merge in next") });
#endif
    removeFromFreeList(next);
    region->incTotalSize(next->getTotalSize());
    if (next != lastRegion) {
      next->next()->prev() = region;
    } else {
      lastRegion = region;
    }
  }
  // We may now be big enough.
  if (size <= getMaxPayload(region)) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.emmalloc_realloc use existing payload space after merge") });
#endif
    region->setUsed(1);
    // There might be enough left over to split out now.
    possiblySplitRemainder(region, size);
    return ptr;
  }
  // We still aren't big enough. If we are the last, we can extend ourselves - however, that
  // definitely means increasing the total sbrk(), and there may be free space lower down, so
  // this is a tradeoff between speed (avoid the memcpy) and space. It's not clear what's
  // better here; for now, check for free space first.
  Region* newRegion = tryFromFreeList(size);
  if (!newRegion && region == lastRegion) {
#ifdef EMMALLOC_DEBUG_LOG
    EM_ASM({ Module.print("  emmalloc.emmalloc_realloc extend last region") });
#endif
    if (extendLastRegion(size)) {
      // It worked. We don't need the formerly free region.
      if (newRegion) {
        stopUsing(newRegion);
      }
      return ptr;
    } else {
      // If this failed, we can also try the normal
      // malloc path, which may find space in a freelist;
      // fall through.
    }
  }
  // We need new space, and a copy
  if (!newRegion) {
    newRegion = newAllocation(size);
    if (!newRegion) return NULL;
  }
  memcpy(getPayload(newRegion), getPayload(region), size < getMaxPayload(region) ? size : getMaxPayload(region));
  stopUsing(region);
  return getPayload(newRegion);
}

static struct mallinfo emmalloc_mallinfo() {
	struct mallinfo info;
  info.arena = 0;
  info.ordblks = 0;
  info.smblks = 0;
  info.hblks = 0;
  info.hblkhd = 0;
  info.usmblks = 0;
  info.fsmblks = 0;
  info.uordblks = 0;
  info.ordblks = 0;
  info.keepcost = 0;
  if (firstRegion) {
    info.arena = (char*)sbrk(0) - (char*)firstRegion;
    Region* region = firstRegion;
    while (region) {
      if (region->getUsed()) {
        info.uordblks += getMaxPayload(region);
      } else {
        info.fordblks += getMaxPayload(region);
        info.ordblks++;
      }
      region = region->next();
    }
  }
  return info;
}

// An aligned allocation. This is a rarer allocation path, and is
// much less optimized - the assumption is that it is used for few
// large allocations.
static void* alignedAllocation(size_t size, size_t alignment) {
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("  emmalloc.alignedAllocation") });
#endif
  assert(alignment > ALIGNMENT);
  assert(alignment % ALIGNMENT == 0);
  // Try from the freelist first. We may be lucky and get something
  // properly aligned.
  // TODO: Perhaps look more carefully, checking alignment as we go,
  //       using multiple tries?
  Region* fromFreeList = tryFromFreeList(size + alignment);
  if (fromFreeList && size_t(getPayload(fromFreeList)) % alignment == 0) {
    // Luck has favored us.
    return getPayload(fromFreeList);
  } else if (fromFreeList) {
    stopUsing(fromFreeList);
  }
  // No luck from free list, so do a new allocation which we can
  // force to be aligned.
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("    emmalloc.alignedAllocation new allocation") });
#endif
  // Ensure a region before us, which we may enlarge as necessary.
  if (!lastRegion) {
    // This allocation is not freeable, but there is one at most.
    void* prev = emmalloc_malloc(MIN_REGION_SIZE);
    if (!prev) return NULL;
  }
  // See if we need to enlarge the previous region in order to get
  // us properly aligned. Take into account that our region will
  // start with METADATA_SIZE of space.
  size_t address = size_t(getAfter(lastRegion)) + METADATA_SIZE;
  size_t error = address % alignment;
  if (error != 0) {
    // E.g. if we want alignment 24, and have address 16, then we
    // need to add 8.
    size_t extra = alignment - error;
    assert(extra % ALIGNMENT == 0);
    if (!extendLastRegion(getMaxPayload(lastRegion) + extra)) {
      return NULL;
    }
    address = size_t(getAfter(lastRegion)) + METADATA_SIZE;
    error = address % alignment;
    assert(error == 0);
  }
  Region* region = allocateRegion(size);
  if (!region) return NULL;
  void* ptr = getPayload(region);
  assert(size_t(ptr) == address);
  assert(size_t(ptr) % alignment == 0);
  return ptr;
}

static int isMultipleOfSizeT(size_t size) {
  return (size & 3) == 0;
}

static int emmalloc_posix_memalign(void **memptr, size_t alignment, size_t size) {
  *memptr = NULL;
  if (!isPowerOf2(alignment) || !isMultipleOfSizeT(alignment)) {
    return 22; // EINVAL
  }
  if (size == 0) {
    return 0;
  }
  if (alignment <= ALIGNMENT) {
    // Use normal allocation path, which will provide that alignment.
    *memptr = emmalloc_malloc(size);
  } else {
    // Use more sophisticaed alignment-specific allocation path.
    *memptr = alignedAllocation(size, alignment);
  }
  if (!*memptr) {
    return 12; // ENOMEM
  }
  return 0;
}

static void* emmalloc_memalign(size_t alignment, size_t size) {
  void* ptr;
  if (emmalloc_posix_memalign(&ptr, alignment, size) != 0) {
    return NULL;
  }
  return ptr;
}

// Public API. This is a thin wrapper around our mirror of it, adding
// logging and validation when debugging. Otherwise it should inline
// out.

extern "C" {

EMMALLOC_EXPORT
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

EMMALLOC_EXPORT
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

EMMALLOC_EXPORT
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

EMMALLOC_EXPORT
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

EMMALLOC_EXPORT
int posix_memalign(void **memptr, size_t alignment, size_t size) {
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.posix_memalign " + [$0, $1, $2]) }, memptr, alignment, size);
#endif
  emmalloc_validate_all();
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
#endif
  int result = emmalloc_posix_memalign(memptr, alignment, size);
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.posix_memalign ==> " + $0) }, result);
#endif
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
  emmalloc_validate_all();
#endif
  return result;
}

EMMALLOC_EXPORT
void* memalign(size_t alignment, size_t size) {
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.memalign " + [$0, $1]) }, alignment, size);
#endif
  emmalloc_validate_all();
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
#endif
  void* ptr = emmalloc_memalign(alignment, size);
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.memalign ==> " + $0) }, ptr);
#endif
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
  emmalloc_validate_all();
#endif
  return ptr;
}

EMMALLOC_EXPORT
struct mallinfo mallinfo() {
#ifdef EMMALLOC_DEBUG
#ifdef EMMALLOC_DEBUG_LOG
  EM_ASM({ Module.print("emmalloc.mallinfo") });
#endif
  emmalloc_validate_all();
#ifdef EMMALLOC_DEBUG_LOG
  emmalloc_dump_all();
#endif
#endif
  return emmalloc_mallinfo();
}

// Export malloc and free as duplicate names emscripten_builtin_malloc and
// emscripten_builtin_free so that applications can replace malloc and free
// in their code, and make those replacements refer to the original malloc
// and free from this file.
// This allows an easy mechanism for hooking into memory allocation.
#if defined(__EMSCRIPTEN__)
extern __typeof(malloc) emscripten_builtin_malloc __attribute__((weak, alias("malloc")));
extern __typeof(free) emscripten_builtin_free __attribute__((weak, alias("free")));
#endif

} // extern "C"
