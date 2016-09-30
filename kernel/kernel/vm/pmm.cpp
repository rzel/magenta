// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "vm_priv.h"
#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <kernel/auto_lock.h>
#include <kernel/mutex.h>
#include <kernel/vm.h>
#include <lib/console.h>
#include <list.h>
#include <new.h>
#include <pow2.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>

#include "pmm_arena.h"

#include <mxtl/intrusive_double_list.h>

#define LOCAL_TRACE MAX(VM_GLOBAL_TRACE, 0)

// the main arena list
static mxtl::DoublyLinkedList<PmmArena*> arena_list;
static Mutex arena_lock;

paddr_t vm_page_to_paddr(const vm_page_t* page) {
    for (const auto& a : arena_list) {
        //LTRACEF("testing page %p against arena %p\n", page, &a);
        if (a.page_belongs_to_arena(page)) {
            return a.page_address_from_arena(page);
        }
    }
    return -1;
}

vm_page_t* paddr_to_vm_page(paddr_t addr) {
    for (auto& a : arena_list) {
        if (a.address_in_arena(addr)) {
            size_t index = (addr - a.base()) / PAGE_SIZE;
            return a.get_page(index);
        }
    }
    return NULL;
}

status_t pmm_add_arena(const pmm_arena_info_t* info) {
    LTRACEF("arena %p name '%s' base %#" PRIxPTR " size %#zx\n", info, info->name, info->base, info->size);

    DEBUG_ASSERT(IS_PAGE_ALIGNED(info->base));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(info->size));
    DEBUG_ASSERT(info->size > 0);

    // allocate a c++ arena object
    PmmArena* arena = new (boot_alloc_mem(sizeof(PmmArena))) PmmArena(info);

    // walk the arena list and add arena based on priority order
    for (auto& a : arena_list) {
        if (a.priority() > arena->priority()) {
            arena_list.insert(a, arena);
            goto done_add;
        }
    }

    // walked off the end, add it to the end of the list
    arena_list.push_back(arena);

done_add:
    // tell the arena to allocate a page array
    arena->BootAllocArray();

    return NO_ERROR;
}

vm_page_t* pmm_alloc_page(uint alloc_flags, paddr_t* pa) {
    AutoLock al(arena_lock);

    /* walk the arenas in order until we find one with a free page */
    for (auto& a : arena_list) {
        /* skip the arena if it's not KMAP and the KMAP only allocation flag was passed */
        if (alloc_flags & PMM_ALLOC_FLAG_KMAP) {
            if ((a.flags() & PMM_ARENA_FLAG_KMAP) == 0)
                continue;
        }

        // try to allocate the page out of the arena
        vm_page_t* page = a.AllocPage(pa);
        if (page)
            return page;
    }

    LTRACEF("failed to allocate page\n");
    return nullptr;
}

size_t pmm_alloc_pages(size_t count, uint alloc_flags, struct list_node* list) {
    LTRACEF("count %zu\n", count);

    /* list must be initialized prior to calling this */
    DEBUG_ASSERT(list);

    size_t allocated = 0;
    if (count == 0)
        return 0;

    AutoLock al(arena_lock);

    /* walk the arenas in order, allocating as many pages as we can from each */
    for (auto& a : arena_list) {
        /* skip the arena if it's not KMAP and the KMAP only allocation flag was passed */
        if (alloc_flags & PMM_ALLOC_FLAG_KMAP) {
            if ((a.flags() & PMM_ARENA_FLAG_KMAP) == 0)
                continue;
        }

        // if it was successful at allocating any amount of pages, return
        allocated = a.AllocPages(count, list);
        if (allocated > 0)
            break;
    }

    return allocated;
}

size_t pmm_alloc_range(paddr_t address, size_t count, struct list_node* list) {
    LTRACEF("address %#" PRIxPTR ", count %zu\n", address, count);

    uint allocated = 0;
    if (count == 0)
        return 0;

    address = ROUNDDOWN(address, PAGE_SIZE);

    AutoLock al(arena_lock);

    /* walk through the arenas, looking to see if the physical page belongs to it */
    for (auto& a : arena_list) {
        while (allocated < count && a.address_in_arena(address)) {
            vm_page_t* page = a.AllocSpecific(address);
            if (!page)
                break;

            if (list)
                list_add_tail(list, &page->free.node);

            allocated++;
            address += PAGE_SIZE;
        }

        if (allocated == count)
            break;
    }

    return allocated;
}

size_t pmm_alloc_contiguous(size_t count, uint alloc_flags, uint8_t alignment_log2, paddr_t* pa,
                            struct list_node* list) {
    LTRACEF("count %zu, align %u\n", count, alignment_log2);

    if (count == 0)
        return 0;
    if (alignment_log2 < PAGE_SIZE_SHIFT)
        alignment_log2 = PAGE_SIZE_SHIFT;

    AutoLock al(arena_lock);

    for (auto& a : arena_list) {
        /* skip the arena if it's not KMAP and the KMAP only allocation flag was passed */
        if (alloc_flags & PMM_ALLOC_FLAG_KMAP) {
            if ((a.flags() & PMM_ARENA_FLAG_KMAP) == 0)
                continue;
        }

        count = a.AllocContiguous(count, alignment_log2, pa, list);
        if (count > 0)
            return count;
    }

    LTRACEF("couldn't find run\n");
    return 0;
}

/* physically allocate a run from arenas marked as KMAP */
void* pmm_alloc_kpages(size_t count, struct list_node* list, paddr_t* _pa) {
    LTRACEF("count %zu\n", count);

    paddr_t pa;
    /* fast path for single count allocations */
    if (count == 1) {
        vm_page_t* p = pmm_alloc_page(PMM_ALLOC_FLAG_KMAP, &pa);
        if (!p)
            return nullptr;

        if (list) {
            list_add_tail(list, &p->free.node);
        }
    } else {
        size_t alloc_count =
            pmm_alloc_contiguous(count, PMM_ALLOC_FLAG_KMAP, PAGE_SIZE_SHIFT, &pa, list);
        if (alloc_count == 0)
            return nullptr;
    }

    LTRACEF("pa %#" PRIxPTR "\n", pa);
    void* ptr = paddr_to_kvaddr(pa);
    DEBUG_ASSERT(ptr);

    if (_pa)
        *_pa = pa;
    return ptr;
}

/* allocate a single page from a KMAP arena and return its virtual address */
void* pmm_alloc_kpage(paddr_t* _pa, vm_page_t** _p) {
    LTRACE_ENTRY;

    paddr_t pa;
    vm_page_t* p = pmm_alloc_page(PMM_ALLOC_FLAG_KMAP, &pa);
    if (!p)
        return nullptr;

    void* ptr = paddr_to_kvaddr(pa);
    DEBUG_ASSERT(ptr);

    if (_pa)
        *_pa = pa;
    if (_p)
        *_p = p;
    return ptr;
}

size_t pmm_free_kpages(void* _ptr, size_t count) {
    LTRACEF("ptr %p, count %zu\n", _ptr, count);

    uint8_t* ptr = (uint8_t*)_ptr;

    struct list_node list;
    list_initialize(&list);

    while (count > 0) {
        vm_page_t* p = paddr_to_vm_page(vaddr_to_paddr(ptr));
        if (p) {
            list_add_tail(&list, &p->free.node);
        }

        ptr += PAGE_SIZE;
        count--;
    }

    return pmm_free(&list);
}

size_t pmm_free(struct list_node* list) {
    LTRACEF("list %p\n", list);

    DEBUG_ASSERT(list);

    AutoLock al(arena_lock);

    uint count = 0;
    while (!list_is_empty(list)) {
        vm_page_t* page = list_remove_head_type(list, vm_page_t, free.node);

        DEBUG_ASSERT(!page_is_free(page));

        /* see which arena this page belongs to and add it */
        for (auto& a : arena_list) {
            if (a.FreePage(page) >= 0) {
                count++;
                break;
            }
        }
    }

    LTRACEF("returning count %u\n", count);

    return count;
}

size_t pmm_free_page(vm_page_t* page) {
    struct list_node list;
    list_initialize(&list);

    list_add_head(&list, &page->free.node);

    return pmm_free(&list);
}

static int cmd_pmm(int argc, const cmd_args* argv) {
    if (argc < 2) {
    notenoughargs:
        printf("not enough arguments\n");
    usage:
        printf("usage:\n");
        printf("%s arenas\n", argv[0].str);
        printf("%s alloc <count>\n", argv[0].str);
        printf("%s alloc_range <address> <count>\n", argv[0].str);
        printf("%s alloc_kpages <count>\n", argv[0].str);
        printf("%s alloc_contig <count> <alignment>\n", argv[0].str);
        printf("%s dump_alloced\n", argv[0].str);
        printf("%s free_alloced\n", argv[0].str);
        return ERR_INTERNAL;
    }

    static struct list_node allocated = LIST_INITIAL_VALUE(allocated);

    if (!strcmp(argv[1].str, "arenas")) {
        for (auto& a : arena_list) {
            a.Dump(false);
        }
    } else if (!strcmp(argv[1].str, "alloc")) {
        if (argc < 3)
            goto notenoughargs;

        struct list_node list;
        list_initialize(&list);

        size_t count = pmm_alloc_pages((uint)argv[2].u, 0, &list);
        printf("alloc returns %zu\n", count);

        vm_page_t* p;
        list_for_every_entry (&list, p, vm_page_t, free.node) {
            printf("\tpage %p, address %#" PRIxPTR "\n", p, vm_page_to_paddr(p));
        }

        /* add the pages to the local allocated list */
        struct list_node* node;
        while ((node = list_remove_head(&list))) {
            list_add_tail(&allocated, node);
        }
    } else if (!strcmp(argv[1].str, "dump_alloced")) {
        vm_page_t* page;

        list_for_every_entry (&allocated, page, vm_page_t, free.node) { dump_page(page); }
    } else if (!strcmp(argv[1].str, "alloc_range")) {
        if (argc < 4)
            goto notenoughargs;

        struct list_node list;
        list_initialize(&list);

        size_t count = pmm_alloc_range(argv[2].u, (uint)argv[3].u, &list);
        printf("alloc returns %zu\n", count);

        vm_page_t* p;
        list_for_every_entry (&list, p, vm_page_t, free.node) {
            printf("\tpage %p, address %#" PRIxPTR "\n", p, vm_page_to_paddr(p));
        }

        /* add the pages to the local allocated list */
        struct list_node* node;
        while ((node = list_remove_head(&list))) {
            list_add_tail(&allocated, node);
        }
    } else if (!strcmp(argv[1].str, "alloc_kpages")) {
        if (argc < 3)
            goto notenoughargs;

        paddr_t pa;
        void* ptr = pmm_alloc_kpages((uint)argv[2].u, NULL, &pa);
        printf("pmm_alloc_kpages returns %p pa %#" PRIxPTR "\n", ptr, pa);
    } else if (!strcmp(argv[1].str, "alloc_contig")) {
        if (argc < 4)
            goto notenoughargs;

        struct list_node list;
        list_initialize(&list);

        paddr_t pa;
        size_t ret = pmm_alloc_contiguous((uint)argv[2].u, 0, (uint8_t)argv[3].u, &pa, &list);
        printf("pmm_alloc_contiguous returns %zu, address %#" PRIxPTR "\n",
               ret, pa);
        printf("address %% align = %#" PRIxPTR "\n",
               static_cast<uintptr_t>(pa % argv[3].u));

        /* add the pages to the local allocated list */
        struct list_node* node;
        while ((node = list_remove_head(&list))) {
            list_add_tail(&allocated, node);
        }
    } else if (!strcmp(argv[1].str, "free_alloced")) {
        size_t err = pmm_free(&allocated);
        printf("pmm_free returns %zu\n", err);
    } else {
        printf("unknown command\n");
        goto usage;
    }

    return NO_ERROR;
}

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 0
STATIC_COMMAND("pmm", "physical memory manager", &cmd_pmm)
#endif
STATIC_COMMAND_END(pmm);
