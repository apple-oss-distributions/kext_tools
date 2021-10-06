#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <mach-o/arch.h>

#include <stdio.h>
#include <fcntl.h>

#include <mach/vm_types.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "fat_util.h"
#include "macho_util.h"

/*******************************************************************************
*
*******************************************************************************/
struct __fat_iterator {
    void              * file_start;
    void              * file_end;
    struct fat_header * fat_header;
    struct fat_arch   * fat_arches;
    uint32_t            num_arches;
    uint32_t            arch_index;

    int unmap    : 1;     // we own the data
    int iterable : 1;     // is fat or a thin mach-o
};

/*******************************************************************************
*
*******************************************************************************/
static int __fat_iterator_init(
    struct __fat_iterator * iter,
    const void * file_data,
    const void * file_end,
    int macho_only)
{
    int result = -1;
    uint32_t magic;

    iter->file_start = (void *)file_data;
    iter->file_end   = (void *)file_end;

    magic = MAGIC32(file_data);

    if (ISFAT(magic)) {
        void * arches_end;

        iter->fat_header = (struct fat_header *)file_data;
        iter->fat_arches = (struct fat_arch *)((char *)iter->fat_header +
            sizeof(struct fat_header));
        iter->num_arches = NXSwapBigLongToHost(
            iter->fat_header->nfat_arch);
        arches_end = (void *)iter->fat_arches +
            (iter->num_arches * sizeof(struct fat_arch));

        if (arches_end > iter->file_end) {

            goto finish;
        }

        iter->iterable = 1;

    } else if (ISMACHO(magic)) {

        iter->iterable = 1;
        iter->num_arches = 1;
        iter->arch_index = 0;

    } else if (macho_only) {
        goto finish;
    }

    result = 0;

finish:
    return result;
}

/*******************************************************************************
*
*******************************************************************************/
fat_iterator fat_iterator_open(const char * path, int macho_only)
{
    struct __fat_iterator * result = NULL;
    struct __fat_iterator local_iter = { 0, };

    int fd = -1;
    struct stat stat_buf;
    vm_address_t file_data = (vm_address_t)MAP_FAILED;    // must munmap()

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        goto finish;
    }

    if (fstat(fd, &stat_buf) == -1) {
        goto finish;
    }

    if (stat_buf.st_size < sizeof(struct mach_header)) {
        goto finish;
    }

    file_data = (vm_address_t)mmap(0, stat_buf.st_size, PROT_READ,
        MAP_FILE|MAP_PRIVATE, fd, 0);
    if (file_data == (vm_address_t)MAP_FAILED) {
        goto finish;
    }

    local_iter.unmap = 1;

    if (-1 == __fat_iterator_init(&local_iter, (char *)file_data,
        (char *)file_data + stat_buf.st_size, macho_only)) {

        goto finish;
    }

    result = (struct __fat_iterator *)malloc(sizeof(struct __fat_iterator));
    if (!result) {
        goto finish;
    }
    bzero(result, sizeof(struct __fat_iterator));
    memcpy(result, &local_iter, sizeof(struct __fat_iterator));

finish:
    if (fd != -1) close(fd);

    if (!result) {
        if (file_data != (vm_address_t)MAP_FAILED) {
            munmap((void *)file_data, stat_buf.st_size);
        }
    }
    return (fat_iterator)result;
}

/*******************************************************************************
*
*******************************************************************************/
fat_iterator fat_iterator_for_data(
    const void * file_data,
    const void * file_end,
    int macho_only)
{
    struct __fat_iterator * result = NULL;
    struct __fat_iterator local_iter = { 0, };

    if (-1 == __fat_iterator_init(&local_iter, file_data,
        file_end, macho_only)) {

        goto finish;
    }

    result = (struct __fat_iterator *)malloc(
        sizeof(struct __fat_iterator));
    if (!result) {
        goto finish;
    }
    bzero(result, sizeof(struct __fat_iterator));
    memcpy(result, &local_iter, sizeof(struct __fat_iterator));

finish:
    return (fat_iterator)result;
}

/*******************************************************************************
*
*******************************************************************************/
void fat_iterator_close(fat_iterator iter)
{

    if (iter->unmap) {
        if (iter->file_start) {
            munmap((void *)iter->file_start, iter->file_end -
                iter->file_start);
        }
    }

    free(iter);

    return;
}

/*******************************************************************************
*
*******************************************************************************/
int fat_iterator_is_iterable(fat_iterator iter)
{
    return iter->iterable;
}

/*******************************************************************************
*
*******************************************************************************/
void * fat_iterator_next_arch(
    fat_iterator iter,
    void ** file_end)
{
    void * result = NULL;

    if (!iter->fat_header) {
        if (iter->arch_index == 0) {
            result = iter->file_start;
            if (file_end) {
                *file_end = iter->file_end;
            }
            iter->arch_index++;
        }
    } else {
        if (iter->arch_index < iter->num_arches) {
            struct fat_arch * arch_start;
            void * arch_end;

            arch_start = (struct fat_arch *)((void *)iter->fat_arches +
                (iter->arch_index * sizeof(struct fat_arch)));

            result = ((void *)iter->file_start +
                arch_start->offset);
            arch_end = (void *)result + arch_start->size;

            if (arch_end > iter->file_end) {
                result = NULL;
                iter->arch_index = iter->num_arches;
                goto finish;
            }

            if (file_end) {
                *file_end = arch_end;
            }

            iter->arch_index++;
        }
    }

finish:
    return result;
}

/*******************************************************************************
*
*******************************************************************************/
void fat_iterator_reset(fat_iterator iter)
{
    iter->arch_index = 0;
    return;
}

/*******************************************************************************
*
*******************************************************************************/
void * fat_iterator_find_arch(
    fat_iterator iter,
    cpu_type_t cputype,
    cpu_subtype_t cpusubtype,
    void ** arch_end_ptr)
{
    uint32_t magic;

    void * arch_start = NULL;
    void * arch_end = NULL;

    uint32_t nfat_arch;

    struct fat_arch * fat_arches;
    struct fat_arch * fat_arches_copy = NULL;  // must free

    struct fat_arch * found_arch;

    magic = MAGIC32(iter->file_start);

    if (iter->fat_header) {
        uint32_t fat_arches_size;
        uint32_t index;

        nfat_arch = iter->num_arches;
        fat_arches_size = nfat_arch * sizeof(struct fat_arch);

        fat_arches_copy = (struct fat_arch *)(malloc(fat_arches_size));
        if (!fat_arches_copy) {
            goto finish;
        }

        fat_arches = fat_arches_copy;

        memcpy(fat_arches, iter->fat_arches, fat_arches_size);

        for (index = 0; index < nfat_arch; index++) {
            fat_arches[index].cputype =
                OSSwapBigToHostInt32(fat_arches[index].cputype);
            fat_arches[index].cpusubtype =
                OSSwapBigToHostInt32(fat_arches[index].cpusubtype);
            fat_arches[index].offset =
                OSSwapBigToHostInt32(fat_arches[index].offset);
            fat_arches[index].size =
                OSSwapBigToHostInt32(fat_arches[index].size);
            fat_arches[index].align =
                OSSwapBigToHostInt32(fat_arches[index].align);
        }
    } else {
        struct fat_arch fake_fat_arches;
        uint8_t  swap;
        struct mach_header * mach_hdr;

        nfat_arch = 1;

        bzero(&fake_fat_arches, sizeof(fake_fat_arches));

        fat_arches = &fake_fat_arches;

        swap = ISSWAPPEDMACHO(magic);
        mach_hdr = (struct mach_header *)iter->file_start;
        fat_arches[0].cputype = CondSwapInt32(swap, mach_hdr->cputype);
        fat_arches[0].cpusubtype = CondSwapInt32(swap, mach_hdr->cpusubtype);

        fat_arches[0].offset = 0;
        fat_arches[0].size = iter->file_end - iter->file_start;
        fat_arches[0].align = 1;  // not used anyhow
    }

    found_arch = NXFindBestFatArch(cputype, cpusubtype, fat_arches, nfat_arch);
    if (found_arch) {
        arch_start = iter->file_start + found_arch->offset;
        arch_end = arch_start + found_arch->size;
    }

finish:
    if (arch_end_ptr) {
        *arch_end_ptr = arch_end;
    }
    if (fat_arches_copy) {
        free(fat_arches_copy);
    }

    return arch_start;
}

/*******************************************************************************
*
*******************************************************************************/
void * fat_iterator_find_host_arch(
    fat_iterator iter,
    void ** arch_end_ptr)
{
    const NXArchInfo * archinfo;

    archinfo = NXGetLocalArchInfo();
    if (!archinfo) {
        return NULL;
    }
    return fat_iterator_find_arch(iter, archinfo->cputype,
        archinfo->cpusubtype, arch_end_ptr);
}

/*******************************************************************************
*
*******************************************************************************/
const void * fat_iterator_file_start(fat_iterator iter)
{
    return iter->file_start;
    return NULL;
}

/*******************************************************************************
*
*******************************************************************************/
const void * fat_iterator_file_end(fat_iterator iter)
{
    return iter->file_end;
    return NULL;
}

