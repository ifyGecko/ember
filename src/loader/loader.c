/*
 * loader.c - ELF and raw image loaders (uam/loader.h).
 *
 * ELF segments are loaded at their physical address (p_paddr), which is
 * what firmware linker scripts use for load-time placement.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/isa.h"
#include "uam/loader.h"

static uam_status read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        uam_log(UAM_LOG_ERROR, "loader", "cannot open %s", path);
        return UAM_ERR_INVALID;
    }
    uam_status st = UAM_OK;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return UAM_ERR_INVALID;
    }
    long sz = ftell(f);
    rewind(f);
    *data = malloc(sz > 0 ? (size_t)sz : 1);
    if (sz < 0 || !*data) {
        st = UAM_ERR_NOMEM;
    } else if (fread(*data, 1, (size_t)sz, f) != (size_t)sz) {
        st = UAM_ERR_INVALID;
    }
    fclose(f);
    if (st != UAM_OK) {
        free(*data);
        *data = NULL;
        return st;
    }
    *len = (size_t)sz;
    return UAM_OK;
}

/* Endian-aware field readers for ELF headers. */
static uint64_t rd(const uint8_t *p, unsigned size, bool be)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)p[be ? size - 1 - i : i] << (8 * i);
    return v;
}

#define PT_LOAD 1

/* Translate a guest load address to physical through the ISA, if any. */
static uam_status load_bytes_xlated(uam_bus *bus, uam_cpu *cpu, uint64_t vaddr,
                                    const void *data, size_t len)
{
    if (!cpu || !cpu->isa || !cpu->isa->translate)
        return uam_bus_load_bytes(bus, vaddr, data, len);
    /* Translate per page, since a segment could in principle cross a mapping
     * boundary; load addresses here are page-aligned region starts in practice. */
    const uint8_t *p = data;
    while (len) {
        uam_xlate xl = { 0 };
        uam_status st = cpu->isa->translate(cpu, vaddr, UAM_XLATE_DEBUG, &xl);
        if (st != UAM_OK) {
            uam_log(UAM_LOG_ERROR, "loader", "load address 0x%llx has no translation",
                    (unsigned long long)vaddr);
            return st;
        }
        uint64_t pa = xl.paddr;
        uint64_t page_end = (vaddr | (UAM_PAGE_SIZE - 1)) + 1;
        size_t chunk = (size_t)(page_end - vaddr);
        if (chunk > len)
            chunk = len;
        st = uam_bus_load_bytes(bus, pa, p, chunk);
        if (st != UAM_OK)
            return st;
        vaddr += chunk;
        p += chunk;
        len -= chunk;
    }
    return UAM_OK;
}

static uam_status load_elf_buf(uam_bus *bus, uam_cpu *cpu, const uint8_t *d, size_t len,
                               uam_image_info *info)
{
    if (len < 52 || memcmp(d, "\177ELF", 4) != 0)
        return UAM_ERR_INVALID;
    const bool is64 = d[4] == 2;
    const bool be = d[5] == 2;
    if ((d[4] != 1 && d[4] != 2) || (d[5] != 1 && d[5] != 2) || (is64 && len < 64))
        return UAM_ERR_INVALID;

    uint64_t entry = is64 ? rd(d + 24, 8, be) : rd(d + 24, 4, be);
    uint64_t phoff = is64 ? rd(d + 32, 8, be) : rd(d + 28, 4, be);
    unsigned phentsize = (unsigned)rd(d + (is64 ? 54 : 42), 2, be);
    unsigned phnum = (unsigned)rd(d + (is64 ? 56 : 44), 2, be);

    memset(info, 0, sizeof(*info));
    info->entry = entry;
    info->elf_machine = (uint16_t)rd(d + 18, 2, be);
    info->elf_class = is64 ? 64 : 32;

    for (unsigned i = 0; i < phnum; i++) {
        uint64_t o = phoff + (uint64_t)i * phentsize;
        if (o + (is64 ? 56u : 32u) > len)
            return UAM_ERR_INVALID;
        const uint8_t *ph = d + o;
        uint32_t type = (uint32_t)rd(ph, 4, be);
        uint64_t off, vaddr, paddr, filesz, memsz;
        if (is64) {
            off = rd(ph + 8, 8, be);  vaddr = rd(ph + 16, 8, be);  paddr = rd(ph + 24, 8, be);
            filesz = rd(ph + 32, 8, be); memsz = rd(ph + 40, 8, be);
        } else {
            off = rd(ph + 4, 4, be);  vaddr = rd(ph + 8, 4, be);  paddr = rd(ph + 12, 4, be);
            filesz = rd(ph + 16, 4, be); memsz = rd(ph + 20, 4, be);
        }
        if (type != PT_LOAD || memsz == 0)
            continue;
        if (off + filesz > len || filesz > memsz)
            return UAM_ERR_INVALID;
        uam_status st = UAM_OK;
        if (filesz)
            st = load_bytes_xlated(bus, cpu, paddr, d + off, (size_t)filesz);
        if (st == UAM_OK && memsz > filesz) {
            uint8_t *zero = calloc(1, (size_t)(memsz - filesz));
            if (!zero)
                return UAM_ERR_NOMEM;
            st = load_bytes_xlated(bus, cpu, paddr + filesz, zero, (size_t)(memsz - filesz));
            free(zero);
        }
        if (st != UAM_OK)
            return st;
        uam_log(UAM_LOG_INFO, "loader", "segment %u: 0x%llx (%llu bytes)", i,
                (unsigned long long)paddr, (unsigned long long)memsz);
        if (!info->n_segments || vaddr < info->link_base)
            info->link_base = vaddr;
        info->n_segments++;
    }
    return info->n_segments ? UAM_OK : UAM_ERR_INVALID;
}

uam_status uam_load_elf(uam_bus *bus, uam_cpu *cpu, const char *path, uam_image_info *info)
{
    uint8_t *d;
    size_t len;
    uam_status st = read_file(path, &d, &len);
    if (st != UAM_OK)
        return st;
    st = load_elf_buf(bus, cpu, d, len, info);
    free(d);
    return st;
}

uam_status uam_load_raw(uam_bus *bus, uam_cpu *cpu, const char *path, uint64_t base, uam_image_info *info)
{
    uint8_t *d;
    size_t len;
    uam_status st = read_file(path, &d, &len);
    if (st != UAM_OK)
        return st;
    st = load_bytes_xlated(bus, cpu, base, d, len);
    free(d);
    memset(info, 0, sizeof(*info));
    info->entry = base;
    info->link_base = base;
    info->n_segments = 1;
    return st;
}

uam_status uam_load_image(uam_bus *bus, uam_cpu *cpu, const char *path, uint64_t raw_base,
                          uam_image_info *info)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        uam_log(UAM_LOG_ERROR, "loader", "cannot open %s", path);
        return UAM_ERR_INVALID;
    }
    unsigned char magic[4] = { 0 };
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    if (n == 4 && memcmp(magic, "\177ELF", 4) == 0)
        return uam_load_elf(bus, cpu, path, info);
    return uam_load_raw(bus, cpu, path, raw_base, info);
}
