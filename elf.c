#include "opsoup.h"

#include <elf.h>

int elf_make_segment_table(image_t *image) {
    segment_t *segment = NULL;
    int nsegs = 0, cur = 0;
    segment_type_t type;
    Elf32_Ehdr *eh;
    Elf32_Shdr *sh;
    char *strings;
    int i;

    eh = (Elf32_Ehdr *) image->core;

    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        fprintf(stderr, "elf: not an ELF image\n");
        return -1;
    }

    if (eh->e_shnum == 0 || eh->e_shstrndx == 0xffff) {
        fprintf(stderr, "elf: no support for ELF images with more than 65535 sections\n");
        return -1;
    }

    if (eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_VERSION] != EV_CURRENT ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_machine != EM_386) {
        fprintf(stderr, "elf: no support for this ELF class (we handle 32-bit LSB, version 1, for i386)\n");
        return -1;
    }

    if (eh->e_type != ET_REL) {
        fprintf(stderr, "elf: no support for ELF types other than 'relocatable'\n");
        return -1;
    }

    sh = (Elf32_Shdr *) (image->core + eh->e_shoff + eh->e_shstrndx * eh->e_shentsize);
    strings = (char *) (image->core + sh->sh_offset);

    for (i = 0; i < eh->e_shnum; i++) {
        sh = (Elf32_Shdr *) (image->core + eh->e_shoff + i * eh->e_shentsize);

        type = seg_NONE;

        switch (sh->sh_type) {
            case SHT_PROGBITS:
                if (!(sh->sh_flags & SHF_ALLOC))
                    break;

                if (sh->sh_flags & SHF_EXECINSTR)
                    type = seg_CODE;
                else
                    type = seg_DATA;
                break;

            case SHT_NOBITS: {
                uint8_t *alloc = malloc(sh->sh_size);
                sh->sh_offset = alloc - o->image.core;

                type = seg_BSS;

                break;
            }

            case SHT_REL:
                type = seg_RELOC;
                break;

            default:
                break;
        }

        if (cur == nsegs) {
            nsegs += 8;
            segment = realloc(segment, sizeof(segment_t) * nsegs);
        }

        segment[cur].name = strings + sh->sh_name;
        segment[cur].type = type;
        segment[cur].start = o->image.core + sh->sh_offset;
        segment[cur].size = sh->sh_size;
        segment[cur].end = segment[cur].start + segment[cur].size;
        segment[cur].info = sh;

        printf("elf: segment '%s' is type seg_%s, start %p, size 0x%x\n", segment[cur].name, type == seg_CODE ? "CODE" : type == seg_DATA ? "DATA" : type == seg_BSS ? "BSS" : type == seg_RELOC ? "RELOC" : "NONE", segment[cur].start, segment[cur].size);

        cur++;
    }

    if (cur == nsegs) {
        nsegs += 1;
        segment = realloc(segment, sizeof(segment_t) * nsegs);
    }

    segment[cur].name = NULL;
    segment[cur].type = seg_NONE;

    image->segment = segment;

    return 0;
}

void elf_load_labels(opsoup_t *o) {
    Elf32_Ehdr *eh;
    Elf32_Shdr *sh;
    char *strings;
    Elf32_Sym *symtab;
    int nsyms, i;
    label_t *l;

    eh = (Elf32_Ehdr *) o->image.core;

    sh = (Elf32_Shdr *) (o->image.core + eh->e_shoff + eh->e_shstrndx * eh->e_shentsize);

    for (i = 0; o->image.segment[i].name != NULL; i++) {
        if (o->image.segment[i].type != seg_NONE)
            continue;

        if (strcmp(o->image.segment[i].name, ".strtab") == 0) {
            strings = (char *) o->image.segment[i].start;
            continue;
        }

        if (strcmp(o->image.segment[i].name, ".symtab") == 0) {
            symtab = (Elf32_Sym *) o->image.segment[i].start;
            nsyms = o->image.segment[i].size / sizeof(Elf32_Sym);
            continue;
        }
    }

    for (i = 0; i < nsyms; i++) {
        if (*(strings + symtab[i].st_name) != '\0') {
            switch (symtab[i].st_shndx) {
                case SHN_UNDEF:
                    l = label_insert((uint8_t *) &symtab[i], label_EXTERN, &o->image.segment[symtab[i].st_shndx]);
                    l->name = strings + symtab[i].st_name;

                    if (o->verbose)
                        printf("  added external name '%s'\n", l->name);
                    break;

                case SHN_ABS:
                case SHN_COMMON:
                    break;

                default:
                    l = label_insert(o->image.segment[symtab[i].st_shndx].start + symtab[i].st_value, label_NAME, &o->image.segment[symtab[i].st_shndx]);
                    l->name = strings + symtab[i].st_name;
                    if (o->verbose)
                        printf("  added name '%s' in section '%s'\n", l->name, l->seg->name);
                    break;
            }
        }
    }
}

int elf_relocate(opsoup_t *o) {
    Elf32_Ehdr *eh;
    char *strings;
    int i, j;
    segment_t *reloc_segment, *target_segment;
    Elf32_Shdr *sh, *shsymtab;
    Elf32_Sym *symtab;
    int nrel;
    Elf32_Rel *rel;
    uint32_t *mem;
    intptr_t val;
    int sreloc = 0;

    eh = (Elf32_Ehdr *) o->image.core;

    sh = (Elf32_Shdr *) (o->image.core + eh->e_shoff + eh->e_shstrndx * eh->e_shentsize);

    for (i = 0; o->image.segment[i].name != NULL; i++) {
        if (o->image.segment[i].type != seg_NONE)
            continue;

        if (strcmp(o->image.segment[i].name, ".strtab") != 0)
            continue;

        strings = (char *) o->image.segment[i].start;
        break;
    }

    if (o->reloc != NULL) {
        free(o->reloc);
        o->reloc = NULL;
        o->nreloc = 0;
    }

    for (i = 0; o->image.segment[i].name != NULL; i++) {
        if (o->image.segment[i].type != seg_RELOC)
            continue;

        reloc_segment = &o->image.segment[i];
        sh = reloc_segment->info;

        rel = (Elf32_Rel *) (o->image.core + sh->sh_offset);
        nrel = sh->sh_size / sh->sh_entsize;

        shsymtab = o->image.segment[sh->sh_link].info;
        symtab = (Elf32_Sym *) (o->image.core + shsymtab->sh_offset);

        target_segment = &o->image.segment[sh->sh_info];
        sh = target_segment->info;

        printf("elf: applying %d relocations from reloc segment '%s' to target segment '%s'\n", nrel, reloc_segment->name, target_segment->name);

        for (j = 0; j < nrel; j++, rel++) {
            Elf32_Sym *sym;
            
            mem = (uint32_t *) (o->image.core + sh->sh_offset + rel->r_offset);

            if (o->nreloc == sreloc) {
                sreloc += 1024;
                o->reloc = (reloc_t *) realloc(o->reloc, sizeof (reloc_t) * sreloc);
            }
            o->reloc[o->nreloc].mem = (uint8_t *) mem;

            sym = &symtab[ELF32_R_SYM(rel->r_info)];

            if (sym->st_shndx == SHN_UNDEF) {
                /* call (e8)*/
                if (((uint8_t *) mem)[-1] == 0xe8)
                    *mem = (uint32_t) sym - 5;
                else
                    *mem = (uint32_t) sym;

                label_insert((uint8_t *) sym, label_EXTERN, target_segment);
                o->reloc[o->nreloc].target = (uint8_t *) sym;
            }

            else {
                val = (intptr_t) o->image.core + ((Elf32_Shdr *) (o->image.segment[sym->st_shndx].info))->sh_offset + sym->st_value;

                switch (ELF32_R_TYPE(rel->r_info)) {
                    case R_386_32:
                        *mem += val;
                        break;

                    case R_386_PC32:
                        *mem += val - (intptr_t) mem;
                        break;

                    default:
                        fprintf(stderr, "elf: unknown relocation type %d\n", ELF32_R_TYPE(rel->r_info));
                        return -1;
                }

                label_insert((uint8_t *) *mem, label_RELOC, target_segment);
                o->reloc[o->nreloc].target = (uint8_t *) *mem;
            }

            o->nreloc++;
        }

        label_print_upgraded("reloc");
    }

    return 0;
}
