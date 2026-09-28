#include <stdint.h>
#include <stddef.h>
#include <logging/print.h>
#include <mm/frame.h>
#include <mm/page.h>
#include <mm/memory.h>
#include <abi/errno.h>
#include <fs/devfs.h>

// The single /dev/shm device and its region table. Regions are intentionally
// never reclaimed on unmap: nothing tracks how many processes still map a
// region, so freeing one on destroy would risk pulling pages out from under a
// live mapping. For a compositor handing out window buffers this is fine -- the
// set is bounded by SHM_MAX_REGIONS and lives for the session.
static shm_dev_t g_shm;

// Round a byte count up to a whole number of pages
static size_t shm_pages_for(size_t size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

shm_region_t *shm_region_find(shm_dev_t *shm, const char *name) {
    if (!shm || !name) return NULL;
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (shm->regions[i].used && strcmp(shm->regions[i].name, name) == 0) {
            return &shm->regions[i];
        }
    }
    return NULL;
}

// mmap addresses a region by index, so an offset of n * PAGE_SIZE selects slot n
shm_region_t *shm_region_by_offset(shm_dev_t *shm, uint64_t offset) {
    if (!shm || (offset % PAGE_SIZE) != 0) return NULL;
    uint64_t index = offset / PAGE_SIZE;
    if (index >= SHM_MAX_REGIONS) return NULL;
    if (!shm->regions[index].used) return NULL;
    return &shm->regions[index];
}

// Reserve a contiguous frame run and publish it under `name`
int shm_region_create(const char *name, size_t size, shm_region_t **out) {
    if (out) *out = NULL;
    if (!name || name[0] == '\0' || size == 0) return -EINVAL;
    if (strlen(name) >= SHM_NAME_LEN) return -EINVAL;

    shm_region_t *existing = shm_region_find(&g_shm, name);
    if (existing) {
        // Re-creating a name that is already live is an error rather than a
        // silent resize: a client holding a mapping of the old region would
        // keep writing into memory the compositor no longer knows about.
        if (out) *out = existing;
        return -EEXIST;
    }

    shm_region_t *slot = NULL;
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (!g_shm.regions[i].used) {
            slot = &g_shm.regions[i];
            break;
        }
    }
    if (!slot) return -ENOMEM;

    size_t pages = shm_pages_for(size);
    if (pages == 0 || pages > UINT32_MAX) return -EINVAL;

    uintptr_t phys = frame_alloc_contig(pages);
    if (phys == 0) {
        print("shm: frame_alloc_contig failed for %lu pages ('%s')\n",
              (unsigned long)pages, name);
        return -ENOMEM;
    }

    slot->phys  = phys;
    slot->pages = (uint32_t)pages;
    slot->size  = pages * PAGE_SIZE;
    slot->used  = 1;
    strncpy(slot->name, name, SHM_NAME_LEN - 1);
    slot->name[SHM_NAME_LEN - 1] = '\0';

    if (out) *out = slot;
    return 0;
}

// Release a region's table slot. The frames are intentionally leaked rather than
// returned to the allocator; see the note above g_shm.
int shm_region_destroy(const char *name) {
    shm_region_t *r = shm_region_find(&g_shm, name);
    if (!r) return -ENOENT;
    r->used  = 0;
    r->phys  = 0;
    r->pages = 0;
    r->size  = 0;
    r->name[0] = '\0';
    return 0;
}

// /dev/shm ioctl: create a region, tear one down, or look one up by name
static int shm_ioctl(devfs_dev_t *dev, unsigned long req, void *arg) {
    shm_dev_t *shm = (shm_dev_t *)dev->priv;
    if (!shm || !arg) return -EINVAL;

    switch (req) {
        case SHM_IOC_CREATE: {
            shm_create_t *c = (shm_create_t *)arg;
            c->name[SHM_NAME_LEN - 1] = '\0';
            shm_region_t *r = NULL;
            int rc = shm_region_create(c->name, c->size, &r);
            if (rc != 0) return rc;
            c->index = (uint64_t)(r - shm->regions);
            return 0;
        }

        case SHM_IOC_DESTROY: {
            char *name = (char *)arg;
            name[SHM_NAME_LEN - 1] = '\0';
            return shm_region_destroy(name);
        }

        case SHM_IOC_QUERY: {
            shm_query_t *q = (shm_query_t *)arg;
            q->found = 0;
            q->index = 0;
            q->size  = 0;
            shm_region_t *r = shm_region_find(shm, q->name);
            if (r) {
                q->found = 1;
                q->index = (uint64_t)(r - shm->regions);
                q->size  = r->size;
            }
            return 0;
        }

        default:
            return -EINVAL;
    }
}

int shm_devfs_init(void) {
    memset(&g_shm, 0, sizeof(g_shm));
    if (devfs_register("shm", NULL, NULL, &g_shm) != 0) return -1;

    devfs_dev_t *dev = devfs_get("shm");
    if (!dev) return -1;
    dev->ioctl     = shm_ioctl;
    dev->mmap_kind = DEVMAP_SHM;
    return 0;
}
