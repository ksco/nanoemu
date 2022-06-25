#include "nanoemu.h"

struct virtio*
virtio_new(uint8_t* disk) {
    struct virtio* virtio = calloc(1, sizeof *virtio);
    virtio->disk = disk;
    virtio->queue_notify = -1;
    return virtio;
}

enum exception
virtio_load(struct virtio* virtio, uint64_t addr, uint64_t size, uint64_t *result) {
    switch (size) {
    case 32:
        switch (addr) {
        case VIRTIO_MAGIC:
            *result = 0x74726976;
            break;
        case VIRTIO_VERSION:
            *result = 0x1;
            break;
        case VIRTIO_DEVICE_ID:
            *result = 0x2;
            break;
        case VIRTIO_VENDOR_ID:
            *result = 0x554d4551;
            break;
        case VIRTIO_DEVICE_FEATURES:
            *result = 0;
            break;
        case VIRTIO_DRIVER_FEATURES:
            *result = virtio->driver_features;
            break;
        case VIRTIO_QUEUE_NUM_MAX:
            *result = 8;
            break;
        case VIRTIO_QUEUE_PFN:
            *result = virtio->queue_pfn;
            break;
        case VIRTIO_STATUS:
            *result = virtio->status;
            break;
        default: *result = 0;
        }
        return OK;
    default:
        return LOAD_ACCESS_FAULT;
    }
}

enum exception
virtio_store(struct virtio* virtio, uint64_t addr, uint64_t size, uint64_t value) {
    switch (size) {
    case 32:
        switch (addr) {
        case VIRTIO_DEVICE_FEATURES:
            virtio->driver_features = value;
            break;
        case VIRTIO_GUEST_PAGE_SIZE:
            virtio->page_size = value;
            break;
        case VIRTIO_QUEUE_SEL:
            virtio->queue_sel = value;
            break;
        case VIRTIO_QUEUE_NUM:
            virtio->queue_num = value;
            break;
        case VIRTIO_QUEUE_PFN:
            virtio->queue_pfn = value;
            break;
        case VIRTIO_QUEUE_NOTIFY:
            virtio->queue_notify = value;
            break;
        case VIRTIO_STATUS:
            virtio->status = value;
            break;
        }
        return OK;
    default:
        return STORE_AMO_ACCESS_FAULT;
    }
}

inline bool
virtio_is_interrupting(struct virtio* virtio) {
    if (virtio->queue_notify != -1) {
        virtio->queue_notify = -1;
        return true;
    }
    return false;
}

inline uint64_t
virtio_desc_addr(struct virtio* virtio) {
    return (uint64_t)virtio->queue_pfn * (uint64_t)virtio->page_size;
}

inline uint64_t
virtio_disk_read(struct virtio* virtio, uint64_t addr) {
    return virtio->disk[addr];
}

inline void
virtio_disk_write(struct virtio* virtio, uint64_t addr, uint64_t value) {
    virtio->disk[addr] = (uint8_t)value;
}

inline uint64_t
virtio_new_id(struct virtio* virtio) {
    virtio->id += 1;
    return virtio->id;
}
