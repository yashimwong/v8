// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/bounded-page-allocator.h"

namespace v8 {
namespace base {

BoundedPageAllocator::BoundedPageAllocator(v8::PageAllocator* page_allocator,
                                           Address start, size_t size,
                                           size_t allocate_page_size)
    : allocate_page_size_(allocate_page_size),
      commit_page_size_(page_allocator->CommitPageSize()),
      page_allocator_(page_allocator),
      region_allocator_(start, size, allocate_page_size_) {
  DCHECK_NOT_NULL(page_allocator);
  DCHECK(IsAligned(allocate_page_size, page_allocator->AllocatePageSize()));
  DCHECK(IsAligned(allocate_page_size_, commit_page_size_));
}

BoundedPageAllocator::Address BoundedPageAllocator::begin() const {
  return region_allocator_.begin();
}

size_t BoundedPageAllocator::size() const { return region_allocator_.size(); }

void* BoundedPageAllocator::AllocatePages(void* hint, size_t size,
                                          size_t alignment,
                                          PageAllocator::Permission access) {
  MutexGuard guard(&mutex_);
  DCHECK(IsAligned(alignment, region_allocator_.page_size()));
  DCHECK(IsAligned(alignment, allocate_page_size_));

  Address address;
  if (alignment <= allocate_page_size_) {
    // TODO(ishell): Consider using randomized version here.
    address = region_allocator_.AllocateRegion(size);
  } else {
    // Currently, this should only be necessary when V8_VIRTUAL_MEMORY_CAGE is
    // enabled, in which case a bounded page allocator is used to allocate WASM
    // memory buffers, which have a larger alignment.
    address = region_allocator_.AllocateAlignedRegion(size, alignment);
  }
  if (address == RegionAllocator::kAllocationFailure) {
    return nullptr;
  }
  CHECK(page_allocator_->SetPermissions(reinterpret_cast<void*>(address), size,
                                        access));
  return reinterpret_cast<void*>(address);
}

bool BoundedPageAllocator::AllocatePagesAt(Address address, size_t size,
                                           PageAllocator::Permission access) {
  DCHECK(IsAligned(address, allocate_page_size_));
  DCHECK(IsAligned(size, allocate_page_size_));

  {
    MutexGuard guard(&mutex_);
    DCHECK(region_allocator_.contains(address, size));

    if (!region_allocator_.AllocateRegionAt(address, size)) {
      return false;
    }
  }

  CHECK(page_allocator_->SetPermissions(reinterpret_cast<void*>(address), size,
                                        access));
  return true;
}

bool BoundedPageAllocator::ReserveForSharedMemoryMapping(void* ptr,
                                                         size_t size) {
  Address address = reinterpret_cast<Address>(ptr);
  DCHECK(IsAligned(address, allocate_page_size_));
  DCHECK(IsAligned(size, commit_page_size_));

  {
    MutexGuard guard(&mutex_);
    DCHECK(region_allocator_.contains(address, size));

    // Region allocator requires page size rather than commit size so just over-
    // allocate there since any extra space couldn't be used anyway.
    size_t region_size = RoundUp(size, allocate_page_size_);
    if (!region_allocator_.AllocateRegionAt(
            address, region_size, RegionAllocator::RegionState::kExcluded)) {
      return false;
    }
  }

  CHECK(page_allocator_->SetPermissions(ptr, size,
                                        PageAllocator::Permission::kNoAccess));
  return true;
}

bool BoundedPageAllocator::FreePages(void* raw_address, size_t size) {
  MutexGuard guard(&mutex_);

  Address address = reinterpret_cast<Address>(raw_address);
  size_t freed_size = region_allocator_.FreeRegion(address);
  if (freed_size != size) return false;
#ifdef V8_VIRTUAL_MEMORY_CAGE
  // When the virtual memory cage is enabled, the pages returned by the
  // BoundedPageAllocator must be zero-initialized, as some of the additional
  // clients expect them to. Decommitting them during FreePages ensures that
  // while also changing the access permissions to kNoAccess.
  CHECK(page_allocator_->DecommitPages(raw_address, size));
#else
  CHECK(page_allocator_->SetPermissions(raw_address, size,
                                        PageAllocator::kNoAccess));
#endif
  return true;
}

bool BoundedPageAllocator::ReleasePages(void* raw_address, size_t size,
                                        size_t new_size) {
  Address address = reinterpret_cast<Address>(raw_address);
  DCHECK(IsAligned(address, allocate_page_size_));

  DCHECK_LT(new_size, size);
  DCHECK(IsAligned(size - new_size, commit_page_size_));

  // Check if we freed any allocatable pages by this release.
  size_t allocated_size = RoundUp(size, allocate_page_size_);
  size_t new_allocated_size = RoundUp(new_size, allocate_page_size_);

#ifdef DEBUG
  {
    // There must be an allocated region at given |address| of a size not
    // smaller than |size|.
    MutexGuard guard(&mutex_);
    DCHECK_EQ(allocated_size, region_allocator_.CheckRegion(address));
  }
#endif

  if (new_allocated_size < allocated_size) {
    MutexGuard guard(&mutex_);
    region_allocator_.TrimRegion(address, new_allocated_size);
  }

  // Keep the region in "used" state just uncommit some pages.
  Address free_address = address + new_size;
  size_t free_size = size - new_size;
#ifdef V8_VIRTUAL_MEMORY_CAGE
  // See comment in FreePages().
  return page_allocator_->DecommitPages(reinterpret_cast<void*>(free_address),
                                        free_size);
#else
  return page_allocator_->SetPermissions(reinterpret_cast<void*>(free_address),
                                         free_size, PageAllocator::kNoAccess);
#endif
}

bool BoundedPageAllocator::SetPermissions(void* address, size_t size,
                                          PageAllocator::Permission access) {
  DCHECK(IsAligned(reinterpret_cast<Address>(address), commit_page_size_));
  DCHECK(IsAligned(size, commit_page_size_));
  DCHECK(region_allocator_.contains(reinterpret_cast<Address>(address), size));
  return page_allocator_->SetPermissions(address, size, access);
}

bool BoundedPageAllocator::DiscardSystemPages(void* address, size_t size) {
  return page_allocator_->DiscardSystemPages(address, size);
}

bool BoundedPageAllocator::DecommitPages(void* address, size_t size) {
  return page_allocator_->DecommitPages(address, size);
}

}  // namespace base
}  // namespace v8
