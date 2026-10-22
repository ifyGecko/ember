/*
 * uam/loader.h - Firmware image loaders.
 *
 * Loaders place image bytes into guest memory through the bus and report the
 * entry point. They do not know about CPUs or boards.
 *
 * Dependencies: bus.h.
 */
#ifndef UAM_LOADER_H
#define UAM_LOADER_H

#include "uam/bus.h"
#include "uam/cpu.h"

typedef struct uam_image_info {
    uint64_t entry;         /* virtual entry address (image's own address space) */
    uint16_t elf_machine;   /* EM_* for ELF images, 0 for raw */
    uint8_t  elf_class;     /* 32 or 64; 0 for raw */
    uint32_t n_segments;
    uint64_t link_base;     /* lowest virtual address of a loaded segment (ELF PT_LOAD
                             * p_vaddr; the base for a raw image): the image's link base */
} uam_image_info;

/*
 * Image load addresses (ELF segment addresses, the raw base) are in the
 * guest's own address space. When `cpu` is non-NULL and its ISA defines a
 * translation, each load address is translated to a physical address before
 * the bytes are written to the bus - so a MIPS image linked at kseg0/kseg1
 * lands in physical RAM. A NULL `cpu` loads at the addresses as given.
 * `info->entry` is always the untranslated (virtual) entry.
 */
uam_status uam_load_elf(uam_bus *bus, uam_cpu *cpu, const char *path, uam_image_info *info);
uam_status uam_load_raw(uam_bus *bus, uam_cpu *cpu, const char *path, uint64_t base, uam_image_info *info);
uam_status uam_load_image(uam_bus *bus, uam_cpu *cpu, const char *path, uint64_t raw_base,
                          uam_image_info *info);

/* Read a whole file into a malloc'd buffer the caller frees (for boards that
 * place images themselves, e.g. into a flash device). */
uam_status uam_load_file(const char *path, uint8_t **data, size_t *len);

#endif /* UAM_LOADER_H */
