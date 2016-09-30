// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

#include <mxtl/intrusive_double_list.h>

#include <trace.h>
#include <kernel/vm.h>

class PmmArena : public mxtl::DoublyLinkedListable<PmmArena*> {
public:
    PmmArena(const pmm_arena_info_t *info);
    ~PmmArena();

    // set up the per page structures, allocated out of the boot time allocator
    void BootAllocArray();

    void Dump(bool dump_pages);

    // accessors
    const pmm_arena_info_t *info() const { return info_; }
    const char *name()  const { return info_->name; }
    paddr_t base() const { return info_->base; }
    size_t size() const { return info_->size; }
    unsigned int flags() const { return info_->flags; }
    unsigned int priority() const { return info_->priority; }

    vm_page_t* get_page(size_t index) { return &page_array_[index]; }

    // main allocation routines
    vm_page_t* AllocPage(paddr_t* pa);
    vm_page_t* AllocSpecific(paddr_t pa);
    size_t AllocPages(size_t count, list_node *list);
    size_t AllocContiguous(size_t count, uint8_t alignment_log2, paddr_t* pa, struct list_node* list);
    status_t FreePage(vm_page_t *page);

    // helpers
    template <class T>
    bool page_belongs_to_arena(T *page) const {
        uintptr_t page_addr = reinterpret_cast<uintptr_t>(page);
        uintptr_t page_array_base = reinterpret_cast<uintptr_t>(page_array_);

        //TRACEF("page_addr 0x%lx, page_array_base 0x%lx\n", page_addr, page_array_base);

        return ((page_addr >= page_array_base) &&
            (page_addr < (page_array_base + (info_->size / PAGE_SIZE) * VM_PAGE_STRUCT_SIZE)));
    }

    template <class T>
    paddr_t page_address_from_arena(T *page) const {
        uintptr_t page_addr = reinterpret_cast<uintptr_t>(page);
        uintptr_t page_array_base = reinterpret_cast<uintptr_t>(page_array_);

        return ((paddr_t)(((page_addr - page_array_base) / VM_PAGE_STRUCT_SIZE) * PAGE_SIZE + info_->base));
    }

    bool address_in_arena(paddr_t address) const {
        return (address >= info_->base && address <= info_->base + info_->size - 1);
    }

private:
    const pmm_arena_info_t *info_ = nullptr;

    vm_page_t* page_array_ = nullptr;

    size_t free_count_ = 0;
    list_node free_list_ = LIST_INITIAL_VALUE(free_list_);
};

