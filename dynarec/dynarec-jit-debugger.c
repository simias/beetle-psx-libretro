#include <stddef.h>
#include <elf.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "dynarec-jit-debugger.h"

/*******************************************************
 * Declarations directly lifted from GDB's info manual *
 *******************************************************/

typedef enum {
   JIT_NOACTION = 0,
   JIT_REGISTER_FN,
   JIT_UNREGISTER_FN
} jit_actions_t;

struct jit_code_entry {
   struct jit_code_entry *next_entry;
   struct jit_code_entry *prev_entry;
   const char *symfile_addr;
   uint64_t symfile_size;
};

struct jit_descriptor {
   uint32_t version;
   /* This type should be jit_actions_t, but we use uint32_t
      to be explicit about the bitwidth.  */
   uint32_t action_flag;
   struct jit_code_entry *relevant_entry;
   struct jit_code_entry *first_entry;
};

/* GDB puts a breakpoint in this function.  */
void __attribute__((noinline)) __jit_debug_register_code() {
}

/* Make sure to specify the version statically, because the
   debugger may check the version before we can set it.  */
struct jit_descriptor __jit_debug_descriptor = { 1, 0, 0, 0 };


/***************************************************************
 * This is lifted from base_jit-main.c in GDB's own test suite *
 ***************************************************************/

/* ElfW is coming from linux. On other platforms it does not exist.
   Let us define it here. */
#ifndef ElfW
# if (defined  (_LP64) || defined (__LP64__))
#   define WORDSIZE 64
# else
#   define WORDSIZE 32
# endif /* _LP64 || __LP64__  */
#define ElfW(type)      _ElfW (Elf, WORDSIZE, type)
#define _ElfW(e,w,t)    _ElfW_1 (e, w, _##t)
#define _ElfW_1(e,w,t)  e##w##t
#endif /* !ElfW  */

/***********************************
 * Custom dynarec code starts here *
 ***********************************/

#define SHSTRTAB_LEN 512

struct elf_data {
   ElfW(Ehdr) header;
   ElfW(Phdr) program_header;
   /* The first section header must be zero (SHN_UNDEF) */
   ElfW(Shdr) dummy_section;
   ElfW(Shdr) text_section;
   ElfW(Shdr) symtab_section;
   ElfW(Shdr) shstrtab_section;
   ElfW(Shdr) strtab_section;
   /* Symbol table */
   ElfW(Sym) symtab[2];
   char shstrtab[SHSTRTAB_LEN];
   char strtab[128];
};

/* Return the current length of the symbol table */
static unsigned shstrtab_len(struct elf_data *elf) {
   unsigned i;

   for (i = 1; i < SHSTRTAB_LEN; i++) {
      if (elf->shstrtab[i - 1] == '\0' && elf->shstrtab[i] == '\0') {
         /* We reached the end of the table */
         break;
      }
   }

   return i;
}

/* Add an entry into the shstrtab and return its index */
static unsigned add_shstrtab_entry(struct elf_data *elf, const char *e) {
   unsigned len = shstrtab_len(elf);

   assert(len + strlen(e) < SHSTRTAB_LEN);
   strcat(&elf->shstrtab[len], e);
   return len;
}

/* Structure containing a full ELF to declare a symbol. */
struct dynarec_symbol {
   struct jit_code_entry jit_entry;
   struct elf_data elf;
};

void dyndebug_add_block(void *start, size_t len, uint32_t block_base) {
   struct dynarec_symbol *s;
   ElfW(Ehdr) *ehdr;
   ElfW(Phdr) *phdr;
   ElfW(Shdr) *symtab;
   ElfW(Shdr) *shstrtab;
   ElfW(Shdr) *strtab;
   ElfW(Shdr) *text;
   ElfW(Sym)  *sym;

   s = calloc(1, sizeof(*s));
   if (s == NULL) {
      /* Not much we can do... */
      abort();
   }

   ehdr = &s->elf.header;
   phdr = &s->elf.program_header;
   text = &s->elf.text_section;
   symtab = &s->elf.symtab_section;
   shstrtab = &s->elf.shstrtab_section;
   strtab = &s->elf.strtab_section;

   /* symtab entry 0 is unused */
   sym = &s->elf.symtab[1];

#ifdef DYNAREC_ARCH_AMD64
   ehdr->e_ident[EI_CLASS] = ELFCLASS64;
   ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
   ehdr->e_machine = EM_X86_64;
#else
   #error "Missing ELF header ident data"
#endif

   ehdr->e_ident[EI_MAG0] = ELFMAG0;
   ehdr->e_ident[EI_MAG1] = ELFMAG1;
   ehdr->e_ident[EI_MAG2] = ELFMAG2;
   ehdr->e_ident[EI_MAG3] = ELFMAG3;

   ehdr->e_ident[EI_VERSION] = EV_CURRENT;

   ehdr->e_ident[EI_OSABI] = ELFOSABI_LINUX;
   ehdr->e_ident[EI_ABIVERSION] = 0;

   ehdr->e_type = ET_EXEC;
   ehdr->e_version = EV_CURRENT;

   ehdr->e_entry = 0;
   ehdr->e_phoff = offsetof(struct elf_data, program_header);
   ehdr->e_shoff = offsetof(struct elf_data, dummy_section);
   ehdr->e_ehsize = sizeof(*ehdr);
   ehdr->e_phentsize = sizeof(*phdr);
   ehdr->e_phnum = 1;
   ehdr->e_shentsize = sizeof(*symtab);
   ehdr->e_shnum = 5;
   ehdr->e_shstrndx = 3;

   /* Program header */
   phdr->p_type = PT_LOAD;
   /* We can't really fill that meaningfully */
   phdr->p_offset = 0;
   /* Virtual address of the recompiled code */
   phdr->p_vaddr = (uintptr_t)start;
   /* Set PSX block start in paddr because why not */
   phdr->p_paddr = (uintptr_t)block_base;
   phdr->p_filesz = 0;
   phdr->p_memsz = len;
   phdr->p_flags = PF_X | PF_W | PF_R;

   text->sh_name = add_shstrtab_entry(&s->elf, ".text");
   text->sh_type = SHT_PROGBITS;
   text->sh_offset = 0;
   text->sh_flags = SHF_EXECINSTR | SHF_ALLOC;
   text->sh_size = len;
   text->sh_addr = (uintptr_t)start;
   text->sh_addralign = 16;

   symtab->sh_name = add_shstrtab_entry(&s->elf, ".symtab");
   symtab->sh_type = SHT_SYMTAB;
   symtab->sh_offset = offsetof(struct elf_data, symtab);
   symtab->sh_size = sizeof(s->elf.symtab);
   symtab->sh_entsize = sizeof(s->elf.symtab[0]);
   symtab->sh_addralign = 1;
   /* One past the index of the last local entry */
   symtab->sh_info = 1;
   /* Index of the string table */
   symtab->sh_link = 4;

   snprintf(&s->elf.strtab[1], sizeof(s->elf.strtab) - 1,
            "block_0x%08x", block_base);

   strtab->sh_name = add_shstrtab_entry(&s->elf, ".strtab");
   strtab->sh_type = SHT_STRTAB;
   strtab->sh_offset = offsetof(struct elf_data, strtab);
   strtab->sh_size = strlen(s->elf.strtab + 1) + 2;
   strtab->sh_addralign = 1;

   shstrtab->sh_name = add_shstrtab_entry(&s->elf, ".shstrtab");
   shstrtab->sh_type = SHT_STRTAB;
   shstrtab->sh_offset = offsetof(struct elf_data, shstrtab);
   shstrtab->sh_size = shstrtab_len(&s->elf);
   shstrtab->sh_addralign = 1;

   /* The symbol's name is the only entry in .strtab*/
   sym->st_name = 1;
   sym->st_value = (uintptr_t)start;
   sym->st_size = len;
   sym->st_info = ELF32_ST_INFO(STB_GLOBAL, STT_FUNC);
   sym->st_other = STV_DEFAULT;
   /* symbol is contained in .text (section index 1) */
   sym->st_shndx = 1;

   s->jit_entry.symfile_addr = (void *)&s->elf;
   s->jit_entry.symfile_size = sizeof(s->elf);
   s->jit_entry.prev_entry = __jit_debug_descriptor.relevant_entry;
   __jit_debug_descriptor.relevant_entry = &s->jit_entry;

   if (s->jit_entry.prev_entry != NULL) {
      s->jit_entry.prev_entry->next_entry = &s->jit_entry;
   } else {
      __jit_debug_descriptor.first_entry = &s->jit_entry;
   }

#if 0
   {
      int fd = open("/tmp/sym.elf", O_WRONLY | O_CREAT| O_TRUNC, 0644);

      write(fd, &s->elf, sizeof(s->elf));

      close(fd);
   }
#endif

   s->jit_entry.symfile_addr = (void *)&s->elf;
   s->jit_entry.symfile_size = sizeof(s->elf);

   __jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
   __jit_debug_register_code();
}

void dyndebug_deregister_all(void) {
   struct jit_code_entry *d = __jit_debug_descriptor.first_entry;

   if (d == NULL) {
      /* Nothing to deregister */
      return;
   }

   __jit_debug_descriptor.relevant_entry = d;
   __jit_debug_descriptor.action_flag = JIT_UNREGISTER_FN;
   __jit_debug_register_code();

   while (d) {
      struct jit_code_entry *next = d->next_entry;

      free(d);

      d = next;
   }

   __jit_debug_descriptor.first_entry = NULL;
   __jit_debug_descriptor.relevant_entry = NULL;
}
