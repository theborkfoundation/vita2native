#include <miniz/miniz.h>
#include <psp2/kernel/error.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "arm-encode.h"
#include "host.h"
#include "jit.h"
#include "load.h"
#include "log.h"
#include "module.h"
#include "sce-elf.h"
#include "self.h"
#include "util.h"

#define IMP_GET_NEXT(imp) ((sce_module_imports_u_t *)((char *)imp + imp->size))
#define IMP_GET_FUNC_COUNT(imp)                                                                    \
    (imp->size == sizeof(sce_module_imports_short_raw) ? imp->imports_short.num_syms_funcs         \
                                                       : imp->imports.num_syms_funcs)
#define IMP_GET_VARS_COUNT(imp)                                                                    \
    (imp->size == sizeof(sce_module_imports_short_raw) ? imp->imports_short.num_syms_vars          \
                                                       : imp->imports.num_syms_vars)
#define IMP_GET_NID(imp)                                                                           \
    (imp->size == sizeof(sce_module_imports_short_raw) ? imp->imports_short.library_nid            \
                                                       : imp->imports.library_nid)
#define IMP_GET_NAME(imp)                                                                          \
    (imp->size == sizeof(sce_module_imports_short_raw) ? imp->imports_short.library_name           \
                                                       : imp->imports.library_name)
#define IMP_GET_FUNC_TABLE(imp)                                                                    \
    (imp->size == sizeof(sce_module_imports_short_raw) ? imp->imports_short.func_nid_table         \
                                                       : imp->imports.func_nid_table)
#define IMP_GET_FUNC_ENTRIES(imp)                                                                  \
    (imp->size == sizeof(sce_module_imports_short_raw) ? imp->imports_short.func_entry_table       \
                                                       : imp->imports.func_entry_table)
#define IMP_GET_VARS_TABLE(imp)                                                                    \
    (imp->size == sizeof(sce_module_imports_short_raw) ? imp->imports_short.var_nid_table          \
                                                       : imp->imports.var_nid_table)
#define IMP_GET_VARS_ENTRIES(imp)                                                                  \
    (imp->size == sizeof(sce_module_imports_short_raw) ? imp->imports_short.var_entry_table        \
                                                       : imp->imports.var_entry_table)

#define CODE_RX_TO_RW_ADDR(rx_base, rw_base, rx_addr)                                              \
    ((((uintptr_t)rx_addr - (uintptr_t)rx_base)) + (uintptr_t)rw_base)

typedef struct {
    void *src_data;
    uintptr_t rw_addr;
    uintptr_t rx_addr; /* Only used for executable segments */
    Elf32_Word p_type;
    Elf32_Addr p_vaddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
    bool needs_free;
} segment_info_t;

static int load_vpk(Jit *jit, const void *data, uint32_t size, void **entry,
                    LoadedModuleInfo *info);
static int load_executable(Jit *jit, const uint8_t *data, void **entry, LoadedModuleInfo *info);

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static int load_elf(Jit *jit, const void *data, void **entry, LoadedModuleInfo *info);
static int load_self(Jit *jit, const void *data, void **entry, LoadedModuleInfo *info);
static int load_segments(Jit *jit, void **entry, Elf32_Addr e_entry, segment_info_t *segments,
                         int num_segments, LoadedModuleInfo *info, bool fixed);
static int resolve_imports(uintptr_t rx_base, uintptr_t rw_base, sce_module_imports_u_t *import,
                           void *importer);
static int relocate(const void *reloc, uint32_t size, segment_info_t *segs, int num_segments);

/* Per-NID resolution logs are very noisy for large titles; unresolved NIDs are
   always reported. */
static bool trace_imports(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_TRACE_IMPORTS") != NULL;

    return enabled != 0;
}

/*
 * Vita userland code is Thumb-2 in practice, but the module info entry points
 * do not carry a Thumb bit in every build: VitaSDK stores them tagged, Sony's
 * and Unity's linkers store plain offsets. Relocation codes are unambiguous
 * (Thumb functions only carry R_ARM_THM_* relocations), so use them to decide.
 * Set VITA2NATIVE_GUEST_ARM=1 to force ARM mode.
 */
static bool module_is_thumb(const segment_info_t *segments, int num_segments)
{
    const char *force_arm = getenv("VITA2NATIVE_GUEST_ARM");
    int thumb = 0;
    int arm = 0;

    if (force_arm && force_arm[0] == '1')
        return false;

    for (int i = 0; i < num_segments && thumb + arm < 512; i++) {
        const uint8_t *reloc;
        uint32_t size;
        uint32_t pos;

        if (segments[i].p_type != PT_SCE_RELA || !segments[i].src_data)
            continue;

        reloc = segments[i].src_data;
        size = segments[i].p_filesz;

        for (pos = 0; pos + 8 <= size;) {
            uint32_t word;
            uint32_t format;
            uint32_t code;

            memcpy(&word, reloc + pos, sizeof(word));
            format = word & 0xF;
            code = (word >> 8) & 0xFF;

            switch (code) {
            case 28: /* R_ARM_CALL */
            case 29: /* R_ARM_JUMP24 */
            case 43: /* R_ARM_MOVW_ABS_NC */
            case 44: /* R_ARM_MOVT_ABS */
            case 45: /* R_ARM_MOVW_PREL_NC */
            case 46: /* R_ARM_MOVT_PREL */
                arm++;
                break;
            case 10:  /* R_ARM_THM_CALL */
            case 30:  /* R_ARM_THM_JUMP24 */
            case 47:  /* R_ARM_THM_MOVW_ABS_NC */
            case 48:  /* R_ARM_THM_MOVT_ABS */
            case 49:  /* R_ARM_THM_MOVW_PREL_NC */
            case 50:  /* R_ARM_THM_MOVT_PREL */
            case 102: /* R_ARM_THM_JUMP11 */
            case 103: /* R_ARM_THM_JUMP8 */
            case 104: /* R_ARM_THM_JUMP6 */
                thumb++;
                break;
            default:
                break;
            }

            pos += (format == 0) ? 12 : 8;
        }
    }

    /* Mixed modules (Unity ships ARM code with Thumb entry wrappers) still
       call their entry points in Thumb mode. */
    return thumb > 0;
}

int elf_check_vita_header(const Elf32_Ehdr *hdr)
{
    if (!(hdr->e_ident[EI_MAG0] == ELFMAG0 && hdr->e_ident[EI_MAG1] == ELFMAG1 &&
          hdr->e_ident[EI_MAG2] == ELFMAG2 && hdr->e_ident[EI_MAG3] == ELFMAG3)) {
        LOG("Invalid ELF magic number.");
        return -1;
    }

    if (hdr->e_ident[EI_CLASS] != ELFCLASS32) {
        LOG("Not a 32bit executable.");
        return -1;
    }

    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        LOG("Not a valid ARM executable.");
        return -1;
    }

    if (hdr->e_ident[EI_VERSION] != EV_CURRENT) {
        LOG("Unsupported ELF version.");
        return -1;
    }

    if (hdr->e_type != ET_SCE_RELEXEC && hdr->e_type != ET_SCE_EXEC) {
        LOG("Unsupported ELF type 0x%x.", (unsigned)hdr->e_type);
        return -1;
    }

    if (hdr->e_machine != EM_ARM) {
        LOG("Not an ARM executable.");
        return -1;
    }

    if (hdr->e_version != EV_CURRENT) {
        LOG("Unsupported ELF version.");
        return -1;
    }

    return 0;
}

static int elf_get_sce_module_info(Elf32_Addr e_entry, const segment_info_t *segments,
                                   sce_module_info_raw **mod_info)
{
    uint32_t offset;
    uint32_t index;

    index = (e_entry & 0xC0000000) >> 30;
    offset = e_entry & 0x3FFFFFFF;

    if (segments[index].src_data == NULL) {
        LOG("Invalid segment index %" PRId32, index);
        return -1;
    }

    *mod_info = (sce_module_info_raw *)((char *)segments[index].src_data + offset);

    return index;
}

int load_file(Jit *jit, const char *filename, void **entry)
{
    return load_file_info(jit, filename, entry, NULL);
}

int load_file_info(Jit *jit, const char *filename, void **entry, LoadedModuleInfo *info)
{
    void *data;
    uint32_t size;
    int ret = 0;

    LOG("Opening '%s' for reading.", filename);
    if (util_load_file(filename, &data, &size) != 0) {
        LOG("Cannot load file.");
        return -1;
    }

    if (load_executable(jit, data, entry, info) < 0) {
        /* Fallback to vpk (zip file) */
        if (load_vpk(jit, data, size, entry, info) < 0) {
            LOG("Unsupported file.");
            ret = -1;
        }
    }

    if (ret == 0 && info) {
        strncpy(info->path, filename, sizeof(info->path) - 1);
        info->entry = *entry;
    }

    free(data);
    return ret;
}

static int load_vpk(Jit *jit, const void *data, uint32_t size, void **entry,
                    LoadedModuleInfo *info)
{
    mz_bool status;
    mz_zip_archive zip_archive;
    size_t uncompressed_size;
    void *eboot_bin;
    int ret;

    mz_zip_zero_struct(&zip_archive);

    status = mz_zip_reader_init_mem(&zip_archive, data, size, 0);
    if (status == MZ_FALSE)
        return -1;

    LOG("Found a ZIP file (VPK), extracting eboot.bin.");

    eboot_bin =
        mz_zip_reader_extract_file_to_heap(&zip_archive, "eboot.bin", &uncompressed_size, 0);
    if (!eboot_bin) {
        mz_zip_reader_end(&zip_archive);
        return -1;
    }

    ret = load_executable(jit, eboot_bin, entry, info);

    mz_free(eboot_bin);
    mz_zip_reader_end(&zip_archive);

    return ret;
}

static int load_executable(Jit *jit, const uint8_t *data, void **entry, LoadedModuleInfo *info)
{
    if (data[0] == ELFMAG0) {
        if (data[1] == ELFMAG1 && data[2] == ELFMAG2 && data[3] == ELFMAG3) {
            LOG("Found an ELF, loading.");
            if (load_elf(jit, data, entry, info) < 0) {
                LOG("Cannot load ELF.");
                return -1;
            }
        }
    } else if (data[0] == SCEMAG0) {
        if (data[1] == SCEMAG1 && data[2] == SCEMAG2 && data[3] == SCEMAG3) {
            LOG("Found a SELF, loading.");
            if (load_self(jit, data, entry, info) < 0) {
                LOG("Cannot load SELF.");
                return -1;
            }
        }
    } else {
        /* Unsupported executable */
        return -1;
    }

    return 0;
}

static int load_self(Jit *jit, const void *data, void **entry, LoadedModuleInfo *info)
{
    const SCE_header *self_header = data;
    const Elf32_Ehdr *elf_hdr = (void *)((char *)data + self_header->elf_offset);
    const Elf32_Phdr *prog_hdrs = (void *)((char *)data + self_header->phdr_offset);
    const segment_info *seg_infos = (void *)((char *)data + self_header->section_info_offset);
    segment_info_t *segments;
    const Elf32_Phdr *seg_header;
    uint8_t *seg_bytes;
    void *uncompressed;
    mz_ulong dest_bytes;
    int ret;

    if (self_header->magic != 0x00454353) {
        LOG("SELF is corrupt or encrypted. Decryption is not yet supported.");
        return -1;
    }

    if (self_header->version != 3) {
        LOG("SELF version 0x%" PRIx32 " is not supported.", self_header->version);
        return -1;
    }

    if (self_header->header_type != 1) {
        LOG("SELF header type 0x%x is not supported.", self_header->header_type);
        return -1;
    }

    LOG("Loading SELF: ELF type: 0x%x, header_type: 0x%x, self_filesize: 0x%llx, self_offset: "
        "0x%llx",
        elf_hdr->e_type, self_header->header_type, self_header->self_filesize,
        self_header->self_offset);

    /* Make sure it contains a PSVita ELF */
    if (elf_check_vita_header(elf_hdr) < 0) {
        LOG("Check header failed.");
        return -1;
    }

    segments = calloc(elf_hdr->e_phnum, sizeof(segment_info_t));
    if (!segments)
        return -1;

    for (Elf32_Half i = 0; i < elf_hdr->e_phnum; i++) {
        seg_header = &prog_hdrs[i];
        seg_bytes = (uint8_t *)data + self_header->header_len + seg_header->p_offset;

        LOG("Segment %i: encryption: %lld, compression: %lld", i, seg_infos[i].encryption,
            seg_infos[i].compression);

        segments[i].p_type = seg_header->p_type;
        segments[i].p_vaddr = seg_header->p_vaddr;
        segments[i].p_filesz = seg_header->p_filesz;
        segments[i].p_memsz = seg_header->p_memsz;
        segments[i].p_flags = seg_header->p_flags;
        segments[i].p_align = seg_header->p_align;

        if (seg_header->p_type == PT_LOAD) {
            if (seg_header->p_memsz != 0) {
                if (seg_infos[i].compression == 2) {
                    dest_bytes = seg_header->p_filesz;
                    uncompressed = malloc(seg_header->p_memsz);
                    if (!uncompressed) {
                        ret = -1;
                        goto done;
                    }

                    ret = mz_uncompress(uncompressed, &dest_bytes,
                                        (uint8_t *)data + seg_infos[i].offset, seg_infos[i].length);
                    if (ret != MZ_OK) {
                        ret = -1;
                        goto done;
                    }

                    segments[i].src_data = uncompressed;
                    segments[i].needs_free = true;
                } else {
                    segments[i].src_data = seg_bytes;
                }
            }
        } else if (seg_header->p_type == PT_LOOS) {
            if (seg_infos[i].compression == 2) {
                dest_bytes = seg_header->p_filesz;
                uncompressed = malloc(seg_header->p_filesz);
                if (!uncompressed) {
                    ret = -1;
                    goto done;
                }

                ret = mz_uncompress(uncompressed, &dest_bytes,
                                    (uint8_t *)data + seg_infos[i].offset, seg_infos[i].length);
                if (ret != MZ_OK) {
                    ret = -1;
                    goto done;
                }

                segments[i].src_data = uncompressed;
                segments[i].needs_free = true;
            } else {
                segments[i].src_data = seg_bytes;
            }
        } else {
            LOG("Unknown segment type 0x%" PRIx32, seg_header->p_type);
        }
    }

    ret = load_segments(jit, entry, elf_hdr->e_entry, segments, elf_hdr->e_phnum, info,
                        elf_hdr->e_type == ET_SCE_EXEC);

done:
    for (Elf32_Half i = 0; i < elf_hdr->e_phnum; i++) {
        if (segments[i].needs_free)
            free(segments[i].src_data);
    }

    free(segments);

    return ret;
}

static int load_elf(Jit *jit, const void *data, void **entry, LoadedModuleInfo *info)
{
    const Elf32_Ehdr *elf_hdr = data;
    Elf32_Phdr *prog_hdrs;
    int ret;

    /* Make sure it's a PSVita ELF */
    if (elf_check_vita_header(elf_hdr) < 0) {
        LOG("Check header failed.");
        return -1;
    }

    LOG("Found %u program segments.", elf_hdr->e_phnum);
    if (elf_hdr->e_phnum < 1) {
        LOG("No program sections to load!");
        return -1;
    }

    /* Read ELF program headers */
    LOG("Reading program headers.");
    prog_hdrs = (void *)((uintptr_t)data + elf_hdr->e_phoff);

    segment_info_t *segments = calloc(elf_hdr->e_phnum, sizeof(segment_info_t));

    for (Elf32_Half i = 0; i < elf_hdr->e_phnum; i++) {
        segments[i].src_data = (char *)data + prog_hdrs[i].p_offset;
        segments[i].p_type = prog_hdrs[i].p_type;
        segments[i].p_vaddr = prog_hdrs[i].p_vaddr;
        segments[i].p_filesz = prog_hdrs[i].p_filesz;
        segments[i].p_memsz = prog_hdrs[i].p_memsz;
        segments[i].p_flags = prog_hdrs[i].p_flags;
        segments[i].p_align = prog_hdrs[i].p_align;
    }

    ret = load_segments(jit, entry, elf_hdr->e_entry, segments, elf_hdr->e_phnum, info,
                        elf_hdr->e_type == ET_SCE_EXEC);

    free(segments);

    return ret;
}

static void unmap_fixed_segments(segment_info_t *segments, int count, size_t page)
{
    for (int i = 0; i < count; i++) {
        uintptr_t base, end;

        if (segments[i].p_type != PT_LOAD || !segments[i].rw_addr)
            continue;

        base = segments[i].p_vaddr & ~(uintptr_t)(page - 1);
        end = (segments[i].p_vaddr + segments[i].p_memsz + page - 1) & ~(uintptr_t)(page - 1);
        munmap((void *)base, (size_t)(end - base));
        segments[i].rw_addr = segments[i].rx_addr = 0;
    }
}

static int load_segments(Jit *jit, void **entry, Elf32_Addr e_entry, segment_info_t *segments,
                         int num_segments, LoadedModuleInfo *info, bool fixed)
{
    uint32_t code_size = 0, data_size = 0;
    uint32_t code_offset = 0, data_offset = 0;
    uintptr_t data_addr, code_rw_addr, code_rx_addr;
    uintptr_t addr_rw, addr_rx;
    Elf32_Word align;
    uint32_t length;
    sce_module_info_raw *mod_info;
    const char *imp_name;
    int mod_info_idx;
    bool module_entry_is_thumb;
    Result res;
    size_t page = 0;

    if (info) {
        memset(info, 0, sizeof(*info));
        info->num_segments = 0;
    }

    /* Calculate total code and data sizes */
    for (int i = 0; i < num_segments; i++) {
        if (segments[i].p_type == PT_LOAD) {
            length = segments[i].p_memsz + segments[i].p_align;
            if ((segments[i].p_flags & PF_X) == PF_X)
                code_size += length;
            else
                data_size += length;
        }
    }

    LOG("Total needed code size: 0x%" PRIx32, code_size);
    LOG("Total needed data size: 0x%" PRIx32, data_size);

    page = (size_t)sysconf(_SC_PAGESIZE);
    if (page == 0)
        page = 4096;

    /*
     * ET_SCE_EXEC executables are linked at a fixed base and carry no
     * relocation table, so every pointer inside them is a guest address
     * (0x81000000 and up). They only work if the segments live at those
     * addresses; the JIT path below rebases everything to host addresses.
     */
    if (fixed) {
        memset(jit, 0, sizeof(*jit));
        jit->fd = -1;

        for (int i = 0; i < num_segments; i++) {
            uintptr_t base, end;
            size_t len;
            void *mapped;

            if (segments[i].p_type != PT_LOAD || !segments[i].p_memsz)
                continue;

            base = segments[i].p_vaddr & ~(uintptr_t)(page - 1);
            end = (segments[i].p_vaddr + segments[i].p_memsz + page - 1) &
                  ~(uintptr_t)(page - 1);
            len = (size_t)(end - base);

            mapped = mmap((void *)base, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (mapped != (void *)base) {
                LOG("Could not map segment %d at %p (got %p)", i, (void *)base, mapped);
                if (mapped != MAP_FAILED)
                    munmap(mapped, len);
                unmap_fixed_segments(segments, i, page);
                return -1;
            }

            segments[i].rx_addr = segments[i].rw_addr = segments[i].p_vaddr;

            if (segments[i].p_filesz)
                memcpy((void *)segments[i].rw_addr, segments[i].src_data, segments[i].p_filesz);
            if (segments[i].p_memsz > segments[i].p_filesz)
                memset((void *)(segments[i].rw_addr + segments[i].p_filesz), 0,
                       segments[i].p_memsz - segments[i].p_filesz);

            LOG("Segment %d mapped at %p (%zu bytes)", i, (void *)segments[i].p_vaddr, len);
        }

        data_addr = 0;
        code_rw_addr = code_rx_addr = segments[0].p_vaddr;
        goto segments_ready;
    }

    res = jitCreate(jit, code_size + 0x1000);
    if (R_FAILED(res)) {
        LOG("jitCreate failed: 0x%" PRIx32, res);
        return -1;
    }

    data_addr = (uintptr_t)malloc(data_size);
    if (!data_addr) {
        LOG("Could not allocate buffer for the data segment");
        goto err_jit_close;
    }

    code_rw_addr = (uintptr_t)jitGetRwAddr(jit);
    code_rx_addr = (uintptr_t)jitGetRxAddr(jit);

    LOG("JIT code RW addr: %p", (void *)code_rw_addr);
    LOG("JIT code RX addr: %p", (void *)code_rx_addr);
    LOG("Data        addr: %p", (void *)data_addr);

    res = jitTransitionToWritable(jit);
    if (R_FAILED(res)) {
        LOG("Could not transition JIT to writable: 0x%" PRIx32, res);
        goto err_free_data;
    }

    for (int i = 0; i < num_segments; i++) {
        if (segments[i].p_type == PT_LOAD) {
            LOG("Found loadable segment (%u)", i);
            LOG("  p_filesz: 0x%" PRIx32, segments[i].p_filesz);
            LOG("  p_memsz:  0x%" PRIx32, segments[i].p_memsz);
            LOG("  p_align:  0x%" PRIx32, segments[i].p_align);

            align = segments[i].p_align ? segments[i].p_align : 4;

            if ((segments[i].p_flags & PF_X) == PF_X) {
                addr_rx = ALIGN(code_rx_addr + code_offset, align);
                addr_rw = code_rw_addr + (addr_rx - code_rx_addr);
                code_offset = (uint32_t)(addr_rx - code_rx_addr) + segments[i].p_memsz;
            } else {
                addr_rw = addr_rx = ALIGN(data_addr + data_offset, align);
                data_offset = (uint32_t)(addr_rw - data_addr) + segments[i].p_memsz;
            }

            segments[i].rx_addr = addr_rx;
            segments[i].rw_addr = addr_rw;

            if (info && info->num_segments < LOAD_MAX_SEGMENTS) {
                LoadedSegment *out = &info->segments[info->num_segments++];

                out->perms = segments[i].p_flags;
                out->vita_vaddr = segments[i].p_vaddr;
                out->vaddr = (void *)addr_rx;
                out->memsz = segments[i].p_memsz;
                out->filesz = segments[i].p_filesz;
            }

            if ((segments[i].p_flags & PF_X) == PF_X) {
                LOG("Code segment loaded at %p for RW, %p for RX", (void *)addr_rw,
                    (void *)addr_rx);
            } else {
                LOG("Data segment loaded at %p ", (void *)addr_rw);
            }

            if (segments[i].p_filesz)
                memcpy((void *)segments[i].rw_addr, segments[i].src_data, segments[i].p_filesz);
            if (segments[i].p_memsz > segments[i].p_filesz)
                memset((char *)segments[i].rw_addr + segments[i].p_filesz, 0,
                       segments[i].p_memsz - segments[i].p_filesz);
        } else if (segments[i].p_type == PT_SCE_RELA) {
            LOG("Found relocations segment (%u)", i);
            relocate(segments[i].src_data, segments[i].p_filesz, segments, num_segments);
        } else {
            LOG("Segment %u is not loadable. Skipping.", i);
            continue;
        }
    }

segments_ready:
    /* Get module info */
    LOG("Getting module info.");
    if ((mod_info_idx = elf_get_sce_module_info(e_entry, segments, &mod_info)) < 0) {
        LOG("Cannot find module info section.");
        goto err_free_data;
    }

    if (fixed) {
        /* Identity mapping: the module already lives at its guest addresses. */
        code_rx_addr = code_rw_addr = segments[mod_info_idx].p_vaddr;
    }

    LOG("Module name: %s", mod_info->name);

    {
        bool thumb = module_is_thumb(segments, num_segments);

        LOG("Module code: %s", thumb ? "Thumb" : "ARM");
        module_entry_is_thumb = thumb;
    }

    if (info) {
        uint8_t *segment_base = (uint8_t *)segments[mod_info_idx].rw_addr;
        uint8_t *loaded_info = segment_base + ((uint8_t *)mod_info -
                                               (uint8_t *)segments[mod_info_idx].src_data);

        info->module_info = loaded_info;
        info->module_segment = segment_base;
        strncpy(info->name, mod_info->name, sizeof(info->name) - 1);
        info->attributes = mod_info->attributes;
        /* exidx/extab offsets are relative to the segment that hosts the
           module info (the code segment); tls_start is relative to the same
           segment's load address. */
        info->exidx_top = mod_info->exidx_top
                              ? (void *)(segments[mod_info_idx].rx_addr + mod_info->exidx_top)
                              : NULL;
        info->exidx_btm = mod_info->exidx_end
                              ? (void *)(segments[mod_info_idx].rx_addr + mod_info->exidx_end)
                              : NULL;
        info->extab_top = mod_info->extab_top
                              ? (void *)(segments[mod_info_idx].rx_addr + mod_info->extab_top)
                              : NULL;
        info->extab_btm = mod_info->extab_end
                              ? (void *)(segments[mod_info_idx].rx_addr + mod_info->extab_end)
                              : NULL;
        info->tls_init = mod_info->tls_start ? (void *)(segment_base + mod_info->tls_start) : NULL;
        info->tls_filesz = mod_info->tls_filesz;
        info->tls_memsz = mod_info->tls_memsz;
        if (mod_info->module_stop != 0 && mod_info->module_stop != 0xFFFFFFFFu)
            info->module_stop = (void *)(segments[mod_info_idx].rx_addr + mod_info->module_stop);
        else
            info->module_stop = NULL;

        if (module_entry_is_thumb && info->module_stop)
            info->module_stop = (void *)((uintptr_t)info->module_stop | 1);

        if (mod_info->export_top && mod_info->export_top != mod_info->export_end) {
            sce_module_exports_raw *export = (void *)(segment_base + mod_info->export_top);
            void *export_end = segment_base + mod_info->export_end;

            while ((void *)export < export_end && export->size) {
                LOG("  Exporting library 0x%08" PRIx32 ": %u funcs, %u vars (nids %p, entries %p)",
                    export->library_nid, export->num_syms_funcs, export->num_syms_vars,
                    (void *)(uintptr_t)export->nid_table,
                    (void *)(uintptr_t)export->entry_table);
                module_register_guest_library(
                    export->library_nid, export->num_syms_funcs, export->num_syms_vars,
                    (const uint32_t *)(uintptr_t)export->nid_table,
                    (const void *const *)(uintptr_t)export->entry_table, loaded_info);
                export = (void *)((uint8_t *)export + export->size);
            }
        }
    }
    LOG("  export table offset: 0x%" PRIx32, mod_info->export_top);
    LOG("  import table offset: 0x%" PRIx32, mod_info->import_top);
    LOG("  tls start: 0x%" PRIx32, mod_info->tls_start);
    LOG("  tls filesz: 0x%" PRIx32, mod_info->tls_filesz);
    LOG("  tls memsz: 0x%" PRIx32, mod_info->tls_memsz);

    /* Resolve NIDs */
    sce_module_imports_u_t *import =
        (void *)(segments[mod_info_idx].rw_addr + mod_info->import_top);
    void *end = (void *)(segments[mod_info_idx].rw_addr + mod_info->import_end);

    for (; (void *)import < end; import = IMP_GET_NEXT(import)) {
        imp_name = (void *)CODE_RX_TO_RW_ADDR(code_rx_addr, code_rw_addr, IMP_GET_NAME(import));
        LOG("Resolving imports for %s (NID: 0x%08" PRIx32 ")", imp_name, IMP_GET_NID(import));
        if (resolve_imports(code_rx_addr, code_rw_addr, import,
                            info ? info->module_info : NULL) < 0) {
            LOG("Failed to resolve imports for %s", imp_name);
            goto err_free_data;
        }
    }

    /* Find the entry point (address belonging to the RX JIT area) */
    *entry = (char *)segments[mod_info_idx].rx_addr + mod_info->module_start;

    if (fixed && !module_entry_is_thumb) {
        const uint16_t *insn = (const uint16_t *)*entry;
        uint16_t first = insn[0];

        /*
         * A fixed executable has no relocations, so module_is_thumb() has
         * nothing to inspect. Entry points start with either an ARM push or a
         * Thumb push/Thumb-2 branch, which is enough to tell them apart.
         */
        if ((first & 0xF800) == 0xF000 || (first & 0xFE00) == 0xB400)
            module_entry_is_thumb = true;
    }

    if (module_entry_is_thumb && *entry)
        *entry = (void *)((uintptr_t)*entry | 1);
    if (*entry == NULL) {
        LOG("Invalid module entry function.");
        goto err_free_data;
    }

    if (fixed) {
        for (int i = 0; i < num_segments; i++) {
            uintptr_t base, end;

            if (segments[i].p_type != PT_LOAD || !(segments[i].p_flags & PF_X))
                continue;

            base = segments[i].p_vaddr & ~(uintptr_t)(page - 1);
            end = (segments[i].p_vaddr + segments[i].p_memsz + page - 1) &
                  ~(uintptr_t)(page - 1);
            __builtin___clear_cache((char *)segments[i].p_vaddr,
                                    (char *)(segments[i].p_vaddr + segments[i].p_memsz));
            /*
             * Keep the code writable: import stubs are re-patched whenever a
             * module the guest imports from is loaded, and the patching writes
             * MOVW/MOVT/BX sequences into the code segment.
             */
            if (mprotect((void *)base, (size_t)(end - base),
                         PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
                LOG("Could not make segment %d executable", i);
                goto err_free_data;
            }
        }
    } else {
        res = jitTransitionToExecutable(jit);
        if (R_FAILED(res)) {
            LOG("Could not transition JIT to executable: 0x%" PRIx32, res);
            goto err_free_data;
        }
    }

    if (info) {
        info->code_rx = (void *)code_rx_addr;
        info->code_rw = (void *)code_rw_addr;
        info->data_addr = (void *)data_addr;
    }

    return 0;

err_free_data:
    if (info && info->module_info) {
        module_remove_importer(info->module_info);
        module_unregister_guest_libraries(info->module_info);
    }
    free((void *)data_addr);
    if (fixed && page)
        unmap_fixed_segments(segments, num_segments, page);
err_jit_close:
    jitClose(jit);

    return -1;
}

void *load_find_export_var(LoadedModuleInfo *info, uint32_t nid)
{
    sce_module_info_raw *mod_info;
    sce_module_exports_raw *export;
    void *export_end;

    if (!info || !info->module_info || !info->module_segment)
        return NULL;

    mod_info = info->module_info;
    if (!mod_info->export_top || mod_info->export_top == mod_info->export_end)
        return NULL;

    export = (void *)((uint8_t *)info->module_segment + mod_info->export_top);
    export_end = (uint8_t *)info->module_segment + mod_info->export_end;

    while ((void *)export < export_end && export->size) {
        uint32_t count = export->num_syms_funcs + export->num_syms_vars;

        for (uint32_t i = 0; i < count; i++) {
            const uint32_t *nids = (const uint32_t *)(uintptr_t)export->nid_table;
            const void *const *entries = (const void *const *)(uintptr_t)export->entry_table;

            if (nids[i] == nid)
                return (void *)(uintptr_t)entries[i];
        }

        export = (void *)((uint8_t *)export + export->size);
    }

    return NULL;
}

int load_repatch_imports(LoadedModuleInfo *info)
{
    sce_module_info_raw *mod_info;
    sce_module_imports_u_t *import;
    void *end;

    if (!info || !info->module_info || !info->module_segment || !info->code_rx || !info->code_rw)
        return -1;

    mod_info = info->module_info;
    import = (void *)((uint8_t *)info->module_segment + mod_info->import_top);
    end = (void *)((uint8_t *)info->module_segment + mod_info->import_end);

    for (; (void *)import < end; import = IMP_GET_NEXT(import)) {
        if (resolve_imports((uintptr_t)info->code_rx, (uintptr_t)info->code_rw, import,
                            info->module_info) < 0)
            return -1;
    }

    return 0;
}

static int resolve_imports(uintptr_t rx_base, uintptr_t rw_base, sce_module_imports_u_t *import,
                           void *importer)
{
    const void *addr;
    uint32_t nid;
    uint32_t *stub;
    uint32_t lib_nid = IMP_GET_NID(import);

    /* The module imports struct contains pointers to entries to the RX JIT area because
     * it has already been relocated, so we have to convert them to RW addresses */

    for (uint32_t i = 0; i < IMP_GET_FUNC_COUNT(import); i++) {
        nid = ((uint32_t *)CODE_RX_TO_RW_ADDR(rx_base, rw_base, IMP_GET_FUNC_TABLE(import)))[i];
        stub = (void *)CODE_RX_TO_RW_ADDR(rx_base, rw_base, IMP_GET_FUNC_ENTRIES(import)[i]);
        addr = module_get_func_export(lib_nid, nid, importer);

        if (addr) {
            if (trace_imports())
                LOG("  Resolved NID 0x%08" PRIx32 " to %p (stub 0x%08" PRIx32 ")", nid, addr,
                    (uint32_t)(rx_base + ((uintptr_t)stub - rw_base)));
            stub[0] = arm_encode_movw(12, (uint16_t)(uintptr_t)addr);
            stub[1] = arm_encode_movt(12, (uint16_t)(((uintptr_t)addr) >> 16));
            stub[2] = arm_encode_bx(12);
        } else {
            stub[0] = arm_encode_movw(0, (uint16_t)SCE_KERNEL_ERROR_MODULEMGR_NO_FUNC_NID);
            stub[1] = arm_encode_movt(0, (uint16_t)(SCE_KERNEL_ERROR_MODULEMGR_NO_FUNC_NID >> 16));
            stub[2] = arm_encode_ret();
            LOG("  Could not resolve NID 0x%08" PRIx32 " (stub at 0x%08" PRIx32
                "), export not found!",
                nid, (uint32_t)(rx_base + ((uintptr_t)stub - rw_base)));
        }
    }

    for (uint32_t i = 0; i < IMP_GET_VARS_COUNT(import); i++) {
        nid = ((uint32_t *)CODE_RX_TO_RW_ADDR(rx_base, rw_base, IMP_GET_VARS_TABLE(import)))[i];
        if (trace_imports())
            LOG("  Trying to resolve variable NID 0x%08" PRIx32, nid);

        addr = module_get_var_export(lib_nid, nid, importer);
        if (addr) {
            uint32_t *entry = (void *)CODE_RX_TO_RW_ADDR(
                rx_base, rw_base, IMP_GET_VARS_ENTRIES(import));
            entry[i] = (uint32_t)(uintptr_t)addr;
        } else {
            LOG("  Could not resolve variable NID 0x%08" PRIx32, nid);
        }
    }

    return 0;
}

/*
 * The SCE relocation stream is a compact, stateful format (formats 0-9, see
 * vita-toolchain's sce-elf.h). Most entries are format 0 (12 bytes, fully
 * specified) or format 1 (8 bytes, the 32-bit offset split across two fields);
 * the rest are deltas against the previous entry.
 *
 * A format 0 entry can patch two locations at once: `code` at `offset` and
 * `code2` at `offset + dist2 * 2` (bytes). Sony's and Unity's toolchains use
 * that to pair a MOVW with its MOVT, so ignoring code2 leaves the high half of
 * every absolute address pointing into the link-time image instead of the JIT.
 */

static void apply_relocation(uint32_t code, uint32_t addend, uintptr_t symval,
                             uintptr_t loc_rw, uintptr_t loc_rx)
{
    int32_t offset;
    uint32_t value = 0;
    uint32_t upper, lower, sign, j1, j2;

    switch (code) {
    case R_ARM_NONE:
        return;
    case R_ARM_V4BX:
        /* Preserve Rm and the condition code. Alter other bits to re-code
           the instruction as MOV PC,Rm. */
        value = (*(uint32_t *)loc_rw & 0xf000000f) | 0x01a0f000;
        break;
    case R_ARM_ABS32:
    case R_ARM_TARGET1:
        value = addend + symval;
        break;
    case R_ARM_REL32:
    case R_ARM_TARGET2:
        value = addend + symval - loc_rx;
        break;
    case R_ARM_THM_PC22: {
        upper = *(uint16_t *)loc_rw;
        lower = *(uint16_t *)(loc_rw + 2);

        /*
         * 25 bit signed address range (Thumb-2 BL and B.W instructions):
         *   S:I1:I2:imm10:imm11:0
         * where:
         *   S     = upper[10]   = offset[24]
         *   I1    = ~(J1 ^ S)   = offset[23]
         *   I2    = ~(J2 ^ S)   = offset[22]
         *   imm10 = upper[9:0]  = offset[21:12]
         *   imm11 = lower[10:0] = offset[11:1]
         *   J1    = lower[13]
         *   J2    = lower[11]
         */
        offset = (int32_t)(addend + symval - loc_rx);

        if (offset <= (int32_t)0xff000000 || offset >= (int32_t)0x01000000) {
            LOG("THM_PC22 relocation out of range: 0x%08" PRIx32, (uint32_t)symval);
            return;
        }

        sign = (offset >> 24) & 1;
        j1 = sign ^ (~(offset >> 23) & 1);
        j2 = sign ^ (~(offset >> 22) & 1);
        upper = (uint16_t)((upper & 0xf800) | (sign << 10) | ((offset >> 12) & 0x03ff));
        lower = (uint16_t)((lower & 0xd000) | (j1 << 13) | (j2 << 11) | ((offset >> 1) & 0x07ff));

        value = ((uint32_t)lower << 16) | upper;
    } break;
    case R_ARM_CALL:
    case R_ARM_JUMP24: {
        offset = (int32_t)(addend + symval - loc_rx);
        if (offset <= (int32_t)0xfe000000 || offset >= (int32_t)0x02000000) {
            LOG("branch relocation out of range: 0x%08" PRIx32, (uint32_t)symval);
            return;
        }

        offset >>= 2;
        offset &= 0x00ffffff;

        value = (*(uint32_t *)loc_rw & 0xff000000) | (uint32_t)offset;
    } break;
    case R_ARM_PREL31:
        offset = (int32_t)(addend + symval - loc_rx);
        value = (uint32_t)offset & 0x7fffffff;
        break;
    case R_ARM_MOVW_ABS_NC:
    case R_ARM_MOVT_ABS: {
        offset = (int32_t)(symval + addend);
        if (code == R_ARM_MOVT_ABS)
            offset >>= 16;

        value = *(uint32_t *)loc_rw;
        value &= 0xfff0f000;
        value |= ((uint32_t)offset & 0xf000) << 4 | ((uint32_t)offset & 0x0fff);
    } break;
    case R_ARM_THM_MOVW_ABS_NC:
    case R_ARM_THM_MOVT_ABS: {
        upper = *(uint16_t *)loc_rw;
        lower = *(uint16_t *)(loc_rw + 2);

        /*
         * MOVT/MOVW instruction encoding in Thumb-2:
         *
         * i    = upper[10]
         * imm4 = upper[3:0]
         * imm3 = lower[14:12]
         * imm8 = lower[7:0]
         *
         * imm16 = imm4:i:imm3:imm8
         */
        offset = (int32_t)(addend + symval);

        if (code == R_ARM_THM_MOVT_ABS)
            offset >>= 16;

        upper = (uint16_t)((upper & 0xfbf0) | (((uint32_t)offset & 0xf000) >> 12) |
                           (((uint32_t)offset & 0x0800) >> 1));
        lower = (uint16_t)((lower & 0x8f00) | (((uint32_t)offset & 0x0700) << 4) |
                           ((uint32_t)offset & 0x00ff));

        value = ((uint32_t)lower << 16) | upper;
    } break;
    default:
        LOG("Unknown relocation code %" PRIu32, code);
        return;
    }

    memcpy((void *)loc_rw, &value, sizeof(value));
}

/* Rebase an ABS32 slot whose current content is a link-time address. */
static void relocate_absolute_slot(uintptr_t loc_rw, uintptr_t loc_rx, const segment_info_t *segs,
                                   int num_segments)
{
    uint32_t original;
    uintptr_t symval = 0;
    uint32_t addend = 0;

    memcpy(&original, (const void *)loc_rw, sizeof(original));

    for (int i = 0; i < num_segments; i++) {
        if (original >= segs[i].p_vaddr && original < segs[i].p_vaddr + segs[i].p_memsz) {
            symval = (uintptr_t)segs[i].rx_addr;
            addend = original - segs[i].p_vaddr;
            break;
        }
    }

    apply_relocation(R_ARM_ABS32, addend, symval, loc_rw, loc_rx);
}

static int relocate(const void *reloc, uint32_t size, segment_info_t *segs, int num_segments)
{
    const uint8_t *bytes = reloc;
    uintptr_t g_addr = 0;
    uintptr_t g_offset = 0;
    uint32_t g_patchseg = 0;
    uint32_t g_saddr = 0;
    uint32_t g_addend = 0;
    uint32_t g_type = 0;
    uint32_t g_type2 = 0;
    uint32_t pos = 0;

    while (pos + 8 <= size) {
        uint32_t w1, w2 = 0, w3 = 0;
        uint32_t format;
        uint32_t step;

        memcpy(&w1, bytes + pos, 4);
        memcpy(&w2, bytes + pos + 4, 4);
        format = w1 & 0xF;
        step = (format == 0) ? 12 : 8;

        if (pos + step > size) {
            LOG("Truncated relocation entry at 0x%" PRIx32, pos);
            break;
        }
        if (step == 12)
            memcpy(&w3, bytes + pos + 8, 4);

        switch (format) {
        case 0: {
            uint32_t symseg = (w1 >> 4) & 0xF;
            uint32_t code = (w1 >> 8) & 0xFF;
            uint32_t patchseg = (w1 >> 16) & 0xF;
            uint32_t code2 = (w1 >> 20) & 0xFF;
            uint32_t dist2 = (w1 >> 28) & 0xF;
            uintptr_t patch_rw, patch_rx, symval;

            if (patchseg >= (uint32_t)num_segments)
                break;

            symval = symseg == 15 ? 0 : (uintptr_t)segs[symseg].rx_addr;
            patch_rw = (uintptr_t)segs[patchseg].rw_addr + w3;
            patch_rx = (uintptr_t)segs[patchseg].rx_addr + w3;

            apply_relocation(code, w2, symval, patch_rw, patch_rx);
            if (code2)
                apply_relocation(code2, w2, symval, patch_rw + dist2 * 2, patch_rx + dist2 * 2);

            g_addr = (uintptr_t)segs[patchseg].rw_addr;
            g_offset = w3;
            g_patchseg = patchseg;
            g_saddr = (uint32_t)symval;
            g_addend = w2;
            g_type = code;
            g_type2 = code2;
        } break;
        case 1: {
            uint32_t symseg = (w1 >> 4) & 0xF;
            uint32_t code = (w1 >> 8) & 0xFF;
            uint32_t patchseg = (w1 >> 16) & 0xF;
            uint32_t offset = (w1 >> 20) | ((w2 & 0x3FF) << 12);
            uint32_t addend = w2 >> 10;
            uint32_t symval;
            uintptr_t patch_rw, patch_rx;

            if (patchseg >= (uint32_t)num_segments)
                break;

            symval = symseg == 15 ? 0 : (uint32_t)(uintptr_t)segs[symseg].rx_addr;
            patch_rw = (uintptr_t)segs[patchseg].rw_addr + offset;
            patch_rx = (uintptr_t)segs[patchseg].rx_addr + offset;

            apply_relocation(code, addend, symval, patch_rw, patch_rx);

            g_addr = (uintptr_t)segs[patchseg].rw_addr;
            g_offset = offset;
            g_patchseg = patchseg;
            g_saddr = symval;
            g_addend = addend;
            g_type = code;
            g_type2 = 0;
        } break;
        case 2: {
            uint32_t symseg = (w1 >> 4) & 0xF;
            uint32_t code = (w1 >> 8) & 0xFF;
            uint32_t offset = (w1 >> 16) & 0xFFFF;
            uint32_t symval;
            uintptr_t patch_rw, patch_rx;

            if (g_patchseg >= (uint32_t)num_segments)
                break;

            g_offset += offset;
            symval = symseg == 15 ? 0 : (uint32_t)(uintptr_t)segs[symseg].rx_addr;
            patch_rw = g_addr + g_offset;
            patch_rx = patch_rw - (uintptr_t)segs[g_patchseg].rw_addr +
                       (uintptr_t)segs[g_patchseg].rx_addr;

            apply_relocation(code, w2, symval, patch_rw, patch_rx);

            g_saddr = symval;
            g_addend = w2;
            g_type = code;
            g_type2 = 0;
        } break;
        case 3: {
            uint32_t symseg = (w1 >> 4) & 0xF;
            uint32_t mode = (w1 >> 8) & 0x1;
            uint32_t offset = (w1 >> 9) & 0x3FFFF;
            uint32_t dist2 = (w1 >> 27) & 0x1F;
            uint32_t addend = w2 & 0x3FFFFF;
            uint32_t symval;
            uintptr_t patch_rw, patch_rx;

            if (g_patchseg >= (uint32_t)num_segments)
                break;

            g_type = mode ? R_ARM_THM_MOVW_ABS_NC : R_ARM_MOVW_ABS_NC;
            g_type2 = mode ? R_ARM_THM_MOVT_ABS : R_ARM_MOVT_ABS;
            g_offset += offset;
            symval = symseg == 15 ? 0 : (uint32_t)(uintptr_t)segs[symseg].rx_addr;
            g_saddr = symval;
            g_addend = addend;

            patch_rw = g_addr + g_offset;
            patch_rx = patch_rw - (uintptr_t)segs[g_patchseg].rw_addr +
                       (uintptr_t)segs[g_patchseg].rx_addr;

            apply_relocation(g_type, addend, symval, patch_rw, patch_rx);
            apply_relocation(g_type2, addend, symval, patch_rw + dist2, patch_rx + dist2);
        } break;
        case 4: {
            uint32_t offset = (w1 >> 4) & 0x7FFFFF;
            uint32_t dist2 = (w1 >> 27) & 0x1F;
            uintptr_t patch_rw, patch_rx;

            if (g_patchseg >= (uint32_t)num_segments)
                break;

            g_offset += offset;
            patch_rw = g_addr + g_offset;
            patch_rx = patch_rw - (uintptr_t)segs[g_patchseg].rw_addr +
                       (uintptr_t)segs[g_patchseg].rx_addr;

            apply_relocation(g_type, g_addend, g_saddr, patch_rw, patch_rx);
            apply_relocation(g_type2, g_addend, g_saddr, patch_rw + dist2, patch_rx + dist2);
        } break;
        case 5: {
            uint32_t dist1 = (w1 >> 4) & 0x1FF;
            uint32_t dist2 = (w1 >> 13) & 0x1F;
            uint32_t dist3 = (w1 >> 18) & 0x1FF;
            uint32_t dist4 = (w1 >> 27) & 0x1F;
            uintptr_t patch_rw, patch_rx;

            if (g_patchseg >= (uint32_t)num_segments)
                break;

            g_offset += dist1;
            patch_rw = g_addr + g_offset;
            patch_rx = patch_rw - (uintptr_t)segs[g_patchseg].rw_addr +
                       (uintptr_t)segs[g_patchseg].rx_addr;
            apply_relocation(g_type, g_addend, g_saddr, patch_rw, patch_rx);
            apply_relocation(g_type2, g_addend, g_saddr, patch_rw + dist2, patch_rx + dist2);

            g_offset += dist3;
            patch_rw = g_addr + g_offset;
            patch_rx = patch_rw - (uintptr_t)segs[g_patchseg].rw_addr +
                       (uintptr_t)segs[g_patchseg].rx_addr;
            apply_relocation(g_type, g_addend, g_saddr, patch_rw, patch_rx);
            apply_relocation(g_type2, g_addend, g_saddr, patch_rw + dist4, patch_rx + dist4);
        } break;
        case 6: {
            uint32_t offset = w1 >> 4;
            uintptr_t patch_rw, patch_rx;

            if (g_patchseg >= (uint32_t)num_segments)
                break;

            g_offset += offset;
            patch_rw = g_addr + g_offset;
            patch_rx = patch_rw - (uintptr_t)segs[g_patchseg].rw_addr +
                       (uintptr_t)segs[g_patchseg].rx_addr;

            relocate_absolute_slot(patch_rw, patch_rx, segs, num_segments);
            g_type = R_ARM_ABS32;
            g_type2 = 0;
        } break;
        case 7:
        case 8:
        case 9: {
            uint32_t offsets = w1 >> 4;
            uint32_t bits = format == 7 ? 7 : (format == 8 ? 4 : 2);
            uint32_t mask = format == 7 ? 0x7F : (format == 8 ? 0x0F : 0x03);

            if (g_patchseg >= (uint32_t)num_segments)
                break;

            do {
                uintptr_t patch_rw, patch_rx;

                g_offset += (offsets & mask) * 4;
                patch_rw = g_addr + g_offset;
                patch_rx = patch_rw - (uintptr_t)segs[g_patchseg].rw_addr +
                           (uintptr_t)segs[g_patchseg].rx_addr;

                relocate_absolute_slot(patch_rw, patch_rx, segs, num_segments);
                offsets >>= bits;
            } while (offsets);

            g_type = R_ARM_ABS32;
            g_type2 = 0;
        } break;
        default:
            LOG("Unsupported relocation entry format %" PRIu32 " at 0x%" PRIx32, format, pos);
            break;
        }

        pos += step;
    }

    return 0;
}
