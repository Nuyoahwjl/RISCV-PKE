/*
 * routines that scan and load a (host) Executable and Linkable Format (ELF) file
 * into the (emulated) memory.
 */

#include "elf.h"
#include "string.h"
#include "riscv.h"
#include "vmm.h"
#include "pmm.h"
#include "util/functions.h"
#include "vfs.h"
#include "spike_interface/spike_file.h"
#include "spike_interface/spike_utils.h"

elf_symbol symbols[64];
char sym_names[64][32];
int sym_count = 0;

typedef struct elf_info_t {
  struct file *f;
  process *p;
} elf_info;

static char g_debug_blob[USER_DEBUG_INFO_SIZE + 16384];

#define USER_DEBUG_STR_SIZE 0x00040000  // 256KB
#define MAX_DEBUG_LINES 600

static char g_debug_line_str[USER_DEBUG_STR_SIZE];
static uint64 g_debug_line_str_len = 0;
static char g_debug_str[USER_DEBUG_STR_SIZE];
static uint64 g_debug_str_len = 0;

enum {
  DW_LNCT_path = 0x1,
  DW_LNCT_directory_index = 0x2,
};

enum {
  DW_FORM_addr = 0x01,
  DW_FORM_block2 = 0x03,
  DW_FORM_block4 = 0x04,
  DW_FORM_data2 = 0x05,
  DW_FORM_data4 = 0x06,
  DW_FORM_data8 = 0x07,
  DW_FORM_string = 0x08,
  DW_FORM_block = 0x09,
  DW_FORM_block1 = 0x0a,
  DW_FORM_data1 = 0x0b,
  DW_FORM_flag = 0x0c,
  DW_FORM_sdata = 0x0d,
  DW_FORM_strp = 0x0e,
  DW_FORM_udata = 0x0f,
  DW_FORM_sec_offset = 0x17,
  DW_FORM_exprloc = 0x18,
  DW_FORM_flag_present = 0x19,
  DW_FORM_line_strp = 0x1f,
};

static void *elf_alloc_mb(elf_ctx *ctx, uint64 elf_pa, uint64 elf_va, uint64 size) {
  (void)elf_pa;
  elf_info *msg = (elf_info *)ctx->info;
  kassert(size < PGSIZE);
  void *pa = alloc_page();
  if (pa == 0) panic("uvmalloc mem alloc falied\n");

  memset((void *)pa, 0, PGSIZE);
  user_vm_map((pagetable_t)msg->p->pagetable, elf_va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ | PROT_EXEC, 1));

  return pa;
}

static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset) {
  elf_info *msg = (elf_info *)ctx->info;
  vfs_lseek(msg->f, offset, SEEK_SET);
  return vfs_read(msg->f, dest, nb);
}

elf_status elf_init(elf_ctx *ctx, void *info) {
  ctx->info = info;

  if (elf_fpread(ctx, &ctx->ehdr, sizeof(ctx->ehdr), 0) != sizeof(ctx->ehdr))
    return EL_EIO;

  if (ctx->ehdr.magic != ELF_MAGIC) return EL_NOTELF;

  return EL_OK;
}

elf_status elf_load(elf_ctx *ctx) {
  elf_prog_header ph_addr;
  int i, off;

  for (i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum;
       i++, off += sizeof(ph_addr)) {
    if (elf_fpread(ctx, (void *)&ph_addr, sizeof(ph_addr), off) != sizeof(ph_addr))
      return EL_EIO;

    if (ph_addr.type != ELF_PROG_LOAD) continue;
    if (ph_addr.memsz < ph_addr.filesz) return EL_ERR;
    if (ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr) return EL_ERR;

    void *dest = elf_alloc_mb(ctx, ph_addr.vaddr, ph_addr.vaddr, ph_addr.memsz);

    if (elf_fpread(ctx, dest, ph_addr.memsz, ph_addr.off) != ph_addr.memsz)
      return EL_EIO;

    int j;
    for (j = 0; j < PGSIZE / sizeof(mapped_region); j++)
      if (((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].va == 0x0) break;

    ((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].va = ph_addr.vaddr;
    ((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].npages = 1;

    if (ph_addr.flags == (SEGMENT_READABLE | SEGMENT_EXECUTABLE)) {
      ((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].seg_type =
          CODE_SEGMENT;
      sprint("CODE_SEGMENT added at mapped info offset:%d\n", j);
    } else if (ph_addr.flags == (SEGMENT_READABLE | SEGMENT_WRITABLE)) {
      ((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].seg_type =
          DATA_SEGMENT;
      sprint("DATA_SEGMENT added at mapped info offset:%d\n", j);
    } else
      panic("unknown program segment encountered, segment flag:%d.\n", ph_addr.flags);

    ((process *)(((elf_info *)(ctx->info))->p))->total_mapped_region++;
  }

  return EL_OK;
}

static int elf_find_section(elf_ctx *ctx, const char *secname, elf_sect_header *out) {
  kassert(ctx->ehdr.shentsize == sizeof(elf_sect_header));

  elf_sect_header shstr;
  uint64 shstr_hdr_off =
      ctx->ehdr.shoff + (uint64)ctx->ehdr.shentsize * ctx->ehdr.shstrndx;
  if (elf_fpread(ctx, &shstr, sizeof(shstr), shstr_hdr_off) != sizeof(shstr)) return -1;

  enum { SHSTRTAB_MAX = 8192 };
  static char shstrtab[SHSTRTAB_MAX];
  if (shstr.size >= SHSTRTAB_MAX) return -1;
  if (elf_fpread(ctx, shstrtab, shstr.size, shstr.offset) != shstr.size) return -1;

  for (uint16 i = 0; i < ctx->ehdr.shnum; i++) {
    elf_sect_header sh;
    uint64 off = ctx->ehdr.shoff + (uint64)ctx->ehdr.shentsize * i;
    if (elf_fpread(ctx, &sh, sizeof(sh), off) != sizeof(sh)) return -1;

    const char *name = (sh.name < shstr.size) ? (shstrtab + sh.name) : "";
    if (strcmp(name, secname) == 0) {
      if (out) *out = sh;
      return 0;
    }
  }
  return -1;
}

static void load_func_name(elf_ctx *ctx) {
  sym_count = 0;
  memset(symbols, 0, sizeof(symbols));
  memset(sym_names, 0, sizeof(sym_names));

  elf_sect_header sh_symtab;
  elf_sect_header sh_strtab;

  if (elf_find_section(ctx, ".symtab", &sh_symtab) != 0) return;
  if (elf_find_section(ctx, ".strtab", &sh_strtab) != 0) return;

  uint64 symnum = sh_symtab.size / sizeof(elf_symbol);
  for (uint64 i = 0; i < symnum && sym_count < 64; i++) {
    elf_symbol symbol_tmp;
    if (elf_fpread(ctx,
                   &symbol_tmp,
                   sizeof(symbol_tmp),
                   sh_symtab.offset + i * sizeof(elf_symbol)) != sizeof(symbol_tmp))
      break;

    // STT_FUNC + STB_GLOBAL commonly appears as 18 in this lab toolchain.
    if (symbol_tmp.st_info == 18 || symbol_tmp.st_info == 2 || symbol_tmp.st_info == 34) {
      if (elf_fpread(ctx,
                     sym_names[sym_count],
                     sizeof(sym_names[sym_count]) - 1,
                     sh_strtab.offset + symbol_tmp.st_name) <= 0)
        continue;
      symbols[sym_count] = symbol_tmp;
      sym_count++;
    }
  }
}

typedef struct {
  uint64 content_type;
  uint64 form;
} line_fmt;

static uint8 read_u8(char **off) {
  uint8 v = (uint8)(**off);
  (*off)++;
  return v;
}

static uint16 read_u16(char **off) {
  uint16 v = (uint16)(uint8)(*off)[0] | ((uint16)(uint8)(*off)[1] << 8);
  (*off) += 2;
  return v;
}

static uint32 read_u32(char **off) {
  uint32 v = 0;
  for (int i = 0; i < 4; i++) v |= ((uint32)(uint8)(*off)[i]) << (i * 8);
  (*off) += 4;
  return v;
}

static uint64 read_u64(char **off) {
  uint64 v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64)(uint8)(*off)[i]) << (i * 8);
  (*off) += 8;
  return v;
}

static uint64 read_offset(char **off, int offset_size) {
  return (offset_size == 8) ? read_u64(off) : (uint64)read_u32(off);
}

static void read_uleb128(uint64 *out, char **off) {
  uint64 value = 0;
  int shift = 0;
  uint8 b;
  for (;;) {
    b = read_u8(off);
    value |= ((uint64)b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0) break;
  }
  if (out) *out = value;
}

static void read_sleb128(int64 *out, char **off) {
  int64 value = 0;
  int shift = 0;
  uint8 b;
  for (;;) {
    b = read_u8(off);
    value |= ((uint64)b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0) break;
  }
  if (shift < 64 && (b & 0x40)) value |= -(1LL << shift);
  if (out) *out = value;
}

static const char *lookup_debug_str(uint64 off) {
  return (off < g_debug_str_len) ? (g_debug_str + off) : 0;
}

static const char *lookup_line_str(uint64 off) {
  return (off < g_debug_line_str_len) ? (g_debug_line_str + off) : 0;
}

static int skip_form(char **off, char *end, uint64 form, int offset_size, int addr_size) {
  uint64 n = 0;
  switch (form) {
    case DW_FORM_data1:
    case DW_FORM_flag:
      n = 1;
      break;
    case DW_FORM_data2:
      n = 2;
      break;
    case DW_FORM_data4:
      n = 4;
      break;
    case DW_FORM_data8:
      n = 8;
      break;
    case DW_FORM_addr:
      n = (uint64)addr_size;
      break;
    case DW_FORM_strp:
    case DW_FORM_line_strp:
    case DW_FORM_sec_offset:
      n = (uint64)offset_size;
      break;
    case DW_FORM_udata:
      read_uleb128(0, off);
      return (*off <= end) ? 0 : -1;
    case DW_FORM_sdata:
      read_sleb128(0, off);
      return (*off <= end) ? 0 : -1;
    case DW_FORM_string:
      while (*off < end && **off) (*off)++;
      if (*off >= end) return -1;
      (*off)++;
      return 0;
    case DW_FORM_block1:
      if (*off + 1 > end) return -1;
      n = read_u8(off);
      break;
    case DW_FORM_block2:
      if (*off + 2 > end) return -1;
      n = read_u16(off);
      break;
    case DW_FORM_block4:
      if (*off + 4 > end) return -1;
      n = read_u32(off);
      break;
    case DW_FORM_block:
    case DW_FORM_exprloc:
      read_uleb128(&n, off);
      break;
    case DW_FORM_flag_present:
      return 0;
    default:
      return -1;
  }

  if (*off + n > end) return -1;
  (*off) += n;
  return 0;
}

static int read_form_uint(uint64 *out,
                          char **off,
                          char *end,
                          uint64 form,
                          int offset_size,
                          int addr_size) {
  uint64 v = 0;
  switch (form) {
    case DW_FORM_data1:
      if (*off + 1 > end) return -1;
      v = read_u8(off);
      break;
    case DW_FORM_data2:
      if (*off + 2 > end) return -1;
      v = read_u16(off);
      break;
    case DW_FORM_data4:
      if (*off + 4 > end) return -1;
      v = read_u32(off);
      break;
    case DW_FORM_data8:
      if (*off + 8 > end) return -1;
      v = read_u64(off);
      break;
    case DW_FORM_udata:
      read_uleb128(&v, off);
      if (*off > end) return -1;
      break;
    case DW_FORM_sdata: {
      int64 s = 0;
      read_sleb128(&s, off);
      if (*off > end) return -1;
      v = (uint64)s;
      break;
    }
    case DW_FORM_addr:
      if (*off + addr_size > end) return -1;
      if (addr_size == 8)
        v = read_u64(off);
      else if (addr_size == 4)
        v = read_u32(off);
      else
        return -1;
      break;
    case DW_FORM_strp:
    case DW_FORM_line_strp:
    case DW_FORM_sec_offset:
      if (*off + offset_size > end) return -1;
      v = read_offset(off, offset_size);
      break;
    default:
      return -1;
  }

  if (out) *out = v;
  return 0;
}

static int read_form_string(const char **out,
                            char **off,
                            char *end,
                            uint64 form,
                            int offset_size) {
  const char *s = 0;
  if (form == DW_FORM_string) {
    char *start = *off;
    while (*off < end && **off) (*off)++;
    if (*off >= end) return -1;
    (*off)++;
    s = start;
  } else if (form == DW_FORM_strp || form == DW_FORM_line_strp) {
    uint64 str_off = 0;
    if (read_form_uint(&str_off, off, end, form, offset_size, 8) != 0) return -1;
    s = (form == DW_FORM_strp) ? lookup_debug_str(str_off) : lookup_line_str(str_off);
  } else {
    return -1;
  }

  if (out) *out = s;
  return 0;
}

static int parse_v5_tables(process *p,
                           char **off,
                           char *prologue_end,
                           int offset_size,
                           int addr_size,
                           int *dir_ind,
                           int *file_ind) {
  if (*off + 1 > prologue_end) return -1;

  int dir_base = *dir_ind;
  uint8 dir_fmt_count = read_u8(off);
  if (dir_fmt_count > 16) return -1;

  line_fmt dir_fmt[16];
  memset(dir_fmt, 0, sizeof(dir_fmt));
  for (int i = 0; i < dir_fmt_count; i++) {
    read_uleb128(&dir_fmt[i].content_type, off);
    read_uleb128(&dir_fmt[i].form, off);
    if (*off > prologue_end) return -1;
  }

  uint64 dir_count = 0;
  read_uleb128(&dir_count, off);
  if (*off > prologue_end) return -1;

  for (uint64 i = 0; i < dir_count; i++) {
    const char *path = 0;
    for (int j = 0; j < dir_fmt_count; j++) {
      if (dir_fmt[j].content_type == DW_LNCT_path) {
        if (read_form_string(&path, off, prologue_end, dir_fmt[j].form, offset_size) != 0)
          return -1;
      } else {
        if (skip_form(off, prologue_end, dir_fmt[j].form, offset_size, addr_size) != 0)
          return -1;
      }
    }

    if (path && *dir_ind < 64) p->dir[(*dir_ind)++] = (char *)path;
  }

  if (*off + 1 > prologue_end) return -1;
  uint8 file_fmt_count = read_u8(off);
  if (file_fmt_count > 16) return -1;

  line_fmt file_fmt[16];
  memset(file_fmt, 0, sizeof(file_fmt));
  for (int i = 0; i < file_fmt_count; i++) {
    read_uleb128(&file_fmt[i].content_type, off);
    read_uleb128(&file_fmt[i].form, off);
    if (*off > prologue_end) return -1;
  }

  uint64 file_count = 0;
  read_uleb128(&file_count, off);
  if (*off > prologue_end) return -1;

  for (uint64 i = 0; i < file_count; i++) {
    const char *path = 0;
    uint64 dir_raw = 0;
    int has_dir = 0;

    for (int j = 0; j < file_fmt_count; j++) {
      if (file_fmt[j].content_type == DW_LNCT_path) {
        if (read_form_string(&path, off, prologue_end, file_fmt[j].form, offset_size) != 0)
          return -1;
      } else if (file_fmt[j].content_type == DW_LNCT_directory_index) {
        if (read_form_uint(&dir_raw,
                           off,
                           prologue_end,
                           file_fmt[j].form,
                           offset_size,
                           addr_size) != 0)
          return -1;
        has_dir = 1;
      } else {
        if (skip_form(off, prologue_end, file_fmt[j].form, offset_size, addr_size) != 0)
          return -1;
      }
    }

    if (*file_ind < 64) {
      p->file[*file_ind].file = (char *)path;
      if (has_dir) {
        uint64 mapped = 0;
        if (dir_raw + dir_base < (uint64)(*dir_ind))
          mapped = dir_raw + dir_base;
        else if (dir_raw > 0 && dir_raw - 1 + dir_base < (uint64)(*dir_ind))
          mapped = dir_raw - 1 + dir_base;
        p->file[*file_ind].dir = mapped;
      } else {
        p->file[*file_ind].dir = 0;
      }
      (*file_ind)++;
    }
  }

  return 0;
}

static void emit_addr_line(process *p, const addr_line *regs, int file_base) {
  if (!p || !regs) return;

  if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs->addr) p->line_ind--;

  if (p->line_ind >= MAX_DEBUG_LINES) return;

  p->line[p->line_ind] = *regs;
  if (p->line[p->line_ind].file > 0)
    p->line[p->line_ind].file += (uint64)file_base - 1;
  else
    p->line[p->line_ind].file = (uint64)file_base;
  p->line_ind++;
}

static const char *find_path_anchor(const char *s, const char *anchor) {
  if (!s || !anchor || !anchor[0]) return 0;

  for (const char *p = s; *p; p++) {
    int i = 0;
    while (anchor[i] && p[i] == anchor[i]) i++;
    if (anchor[i] == 0) return p;
  }
  return 0;
}

static const char *normalize_src_path(const char *path) {
  if (!path) return path;

  while (path[0] == '.' && path[1] == '/') path += 2;

  const char *u = find_path_anchor(path, "user/");
  const char *k = find_path_anchor(path, "kernel/");
  if (u && (!k || u < k)) return u;
  if (k) return k;
  return path;
}

static void make_addr_line(elf_ctx *ctx, char *debug_line, uint64 length) {
  process *p = ((elf_info *)ctx->info)->p;
  p->debugline = debug_line;

  uintptr_t pos = ((uintptr_t)debug_line + length + 7) & ~((uintptr_t)7);
  p->dir = (char **)pos;
  p->file = (code_file *)(p->dir + 64);
  p->line = (addr_line *)(p->file + 64);
  p->line_ind = 0;

  memset(p->dir, 0, sizeof(char *) * 64);
  memset(p->file, 0, sizeof(code_file) * 64);

  int dir_ind = 0;
  int file_ind = 0;

  char *off = debug_line;
  char *end = debug_line + length;

  while (off < end) {
    if (off + 4 > end) break;

    int offset_size = 4;
    uint64 unit_length = read_u32(&off);
    if (unit_length == 0xffffffffU) {
      if (off + 8 > end) break;
      unit_length = read_u64(&off);
      offset_size = 8;
    }

    if (unit_length == 0 || off + unit_length > end) break;
    char *unit_end = off + unit_length;

    if (off + 2 > unit_end) {
      off = unit_end;
      continue;
    }

    uint16 version = read_u16(&off);
    uint8 address_size = 8;
    uint8 min_instruction_length = 1;
    uint8 max_ops_per_instruction = 1;
    uint8 default_is_stmt = 1;
    int8 line_base = -5;
    uint8 line_range = 14;
    uint8 opcode_base = 13;
    uint8 std_opcode_len[256];
    memset(std_opcode_len, 0, sizeof(std_opcode_len));

    int file_base = file_ind;
    int dir_base = dir_ind;

    if (version >= 5) {
      if (off + 2 > unit_end) {
        off = unit_end;
        continue;
      }
      address_size = read_u8(&off);
      (void)read_u8(&off);  // segment_selector_size

      uint64 header_length = read_offset(&off, offset_size);
      char *prologue_end = off + header_length;
      if (prologue_end > unit_end || off + 6 > prologue_end) {
        off = unit_end;
        continue;
      }

      min_instruction_length = read_u8(&off);
      max_ops_per_instruction = read_u8(&off);
      if (max_ops_per_instruction == 0) max_ops_per_instruction = 1;
      default_is_stmt = read_u8(&off);
      line_base = (int8)read_u8(&off);
      line_range = read_u8(&off);
      opcode_base = read_u8(&off);

      for (int i = 1; i < opcode_base && off < prologue_end; i++) std_opcode_len[i] = read_u8(&off);

      if (parse_v5_tables(p,
                          &off,
                          prologue_end,
                          offset_size,
                          address_size,
                          &dir_ind,
                          &file_ind) != 0) {
        off = unit_end;
        continue;
      }
      off = prologue_end;
    } else {
      if (off + offset_size > unit_end) {
        off = unit_end;
        continue;
      }
      uint64 header_length = read_offset(&off, offset_size);
      char *prologue_end = off + header_length;
      if (prologue_end > unit_end || off + 5 > prologue_end) {
        off = unit_end;
        continue;
      }

      min_instruction_length = read_u8(&off);
      default_is_stmt = read_u8(&off);
      line_base = (int8)read_u8(&off);
      line_range = read_u8(&off);
      opcode_base = read_u8(&off);

      for (int i = 1; i < opcode_base && off < prologue_end; i++) std_opcode_len[i] = read_u8(&off);

      while (off < prologue_end && *off != 0) {
        if (dir_ind < 64) p->dir[dir_ind++] = off;
        while (off < prologue_end && *off != 0) off++;
        if (off < prologue_end) off++;
      }
      if (off < prologue_end) off++;

      while (off < prologue_end && *off != 0) {
        const char *fname = off;
        while (off < prologue_end && *off != 0) off++;
        if (off < prologue_end) off++;

        uint64 dir_raw = 0;
        read_uleb128(&dir_raw, &off);

        if (file_ind < 64) {
          uint64 mapped = 0;
          if (dir_raw > 0) {
            if (dir_raw - 1 + dir_base < (uint64)dir_ind)
              mapped = dir_raw - 1 + dir_base;
            else if (dir_raw + dir_base < (uint64)dir_ind)
              mapped = dir_raw + dir_base;
          }
          p->file[file_ind].file = (char *)fname;
          p->file[file_ind].dir = mapped;
          file_ind++;
        }

        read_uleb128(0, &off);
        read_uleb128(0, &off);
      }
      if (off < prologue_end) off++;

      off = prologue_end;
    }

    (void)max_ops_per_instruction;
    (void)default_is_stmt;

    addr_line regs;
    regs.addr = 0;
    regs.file = 1;
    regs.line = 1;

    while (off < unit_end) {
      uint8 op = read_u8(&off);

      if (op == 0) {
        uint64 ext_len = 0;
        read_uleb128(&ext_len, &off);
        if (off + ext_len > unit_end || ext_len == 0) {
          off = unit_end;
          break;
        }

        char *ext_end = off + ext_len;
        uint8 subop = read_u8(&off);
        switch (subop) {
          case 1:  // DW_LNE_end_sequence
            emit_addr_line(p, &regs, file_base);
            regs.addr = 0;
            regs.file = 1;
            regs.line = 1;
            break;
          case 2:  // DW_LNE_set_address
            if (address_size == 8 && off + 8 <= ext_end)
              regs.addr = read_u64(&off);
            else if (address_size == 4 && off + 4 <= ext_end)
              regs.addr = read_u32(&off);
            break;
          case 4:  // DW_LNE_set_discriminator
            read_uleb128(0, &off);
            break;
          default:
            break;
        }
        off = ext_end;
        continue;
      }

      if (op < opcode_base) {
        switch (op) {
          case 1:  // DW_LNS_copy
            emit_addr_line(p, &regs, file_base);
            break;
          case 2: {  // DW_LNS_advance_pc
            uint64 delta = 0;
            read_uleb128(&delta, &off);
            regs.addr += delta * min_instruction_length;
            break;
          }
          case 3: {  // DW_LNS_advance_line
            int64 delta = 0;
            read_sleb128(&delta, &off);
            regs.line += delta;
            break;
          }
          case 4:  // DW_LNS_set_file
            read_uleb128(&regs.file, &off);
            break;
          case 5:  // DW_LNS_set_column
            read_uleb128(0, &off);
            break;
          case 6:  // DW_LNS_negate_stmt
          case 7:  // DW_LNS_set_basic_block
          case 10: // DW_LNS_set_prologue_end
          case 11: // DW_LNS_set_epilogue_begin
            break;
          case 8: {  // DW_LNS_const_add_pc
            if (line_range == 0) break;
            int adjust = 255 - opcode_base;
            int delta = (adjust / line_range) * min_instruction_length;
            regs.addr += delta;
            break;
          }
          case 9: {  // DW_LNS_fixed_advance_pc
            if (off + 2 > unit_end) {
              off = unit_end;
              break;
            }
            regs.addr += read_u16(&off);
            break;
          }
          case 12:  // DW_LNS_set_isa
            read_uleb128(0, &off);
            break;
          default:
            for (int i = 0; i < std_opcode_len[op]; i++) read_uleb128(0, &off);
            break;
        }
      } else {
        if (line_range == 0) continue;
        int adjust = (int)op - (int)opcode_base;
        int addr_delta = (adjust / line_range) * min_instruction_length;
        int line_delta = line_base + (adjust % line_range);
        regs.addr += (uint64)addr_delta;
        regs.line += line_delta;
        emit_addr_line(p, &regs, file_base);
      }

      if (off > unit_end) {
        off = unit_end;
        break;
      }
    }

    off = unit_end;
  }
}

static void load_debug_line(elf_ctx *ctx) {
  process *p = ((elf_info *)ctx->info)->p;
  p->debugline = 0;
  p->dir = 0;
  p->file = 0;
  p->line = 0;
  p->line_ind = 0;

  g_debug_str_len = 0;
  g_debug_line_str_len = 0;

  elf_sect_header dbg;
  if (elf_find_section(ctx, ".debug_line", &dbg) != 0) return;
  if (dbg.size == 0 || dbg.size > USER_DEBUG_INFO_SIZE) return;

  elf_sect_header dbg_line_str;
  if (elf_find_section(ctx, ".debug_line_str", &dbg_line_str) == 0 &&
      dbg_line_str.size > 0 && dbg_line_str.size <= USER_DEBUG_STR_SIZE) {
    memset(g_debug_line_str, 0, sizeof(g_debug_line_str));
    if (elf_fpread(ctx, g_debug_line_str, dbg_line_str.size, dbg_line_str.offset) ==
        dbg_line_str.size)
      g_debug_line_str_len = dbg_line_str.size;
  }

  elf_sect_header dbg_str;
  if (elf_find_section(ctx, ".debug_str", &dbg_str) == 0 && dbg_str.size > 0 &&
      dbg_str.size <= USER_DEBUG_STR_SIZE) {
    memset(g_debug_str, 0, sizeof(g_debug_str));
    if (elf_fpread(ctx, g_debug_str, dbg_str.size, dbg_str.offset) == dbg_str.size)
      g_debug_str_len = dbg_str.size;
  }

  memset(g_debug_blob, 0, sizeof(g_debug_blob));
  if (elf_fpread(ctx, g_debug_blob, dbg.size, dbg.offset) != dbg.size) return;

  make_addr_line(ctx, g_debug_blob, dbg.size);
}
int load_bincode_from_path(process *p, const char *filename) {
  if (filename == NULL || filename[0] == '\0') return -1;

  sprint("Application: %s\n", filename);

  elf_ctx elfloader;
  elf_info info;

  info.f = vfs_open(filename, O_RDONLY);
  info.p = p;
  if (info.f == NULL) {
    sprint("Fail on opening the input application program: %s\n", filename);
    return -1;
  }

  if (elf_init(&elfloader, &info) != EL_OK) {
    vfs_close(info.f);
    return -1;
  }

  if (elf_load(&elfloader) != EL_OK) {
    vfs_close(info.f);
    return -1;
  }

  // challenge hooks
  load_func_name(&elfloader);
  load_debug_line(&elfloader);

  p->trapframe->epc = elfloader.ehdr.entry;
  vfs_close(info.f);

  sprint("Application program entry point (virtual address): 0x%lx\n",
         p->trapframe->epc);
  return 0;
}

void load_bincode_from_host_elf(process *p, char *filename) {
  if (load_bincode_from_path(p, filename) < 0)
    panic("Fail on opening/loading the input application program.\n");
}

static addr_line *lookup_addr_line(process *p, uint64 fault_pc) {
  if (!p || !p->line || p->line_ind <= 0) return 0;
  addr_line *best = 0;
  for (int i = 0; i < p->line_ind; i++) {
    if (p->line[i].addr <= fault_pc) {
      if (!best || p->line[i].addr > best->addr) best = &p->line[i];
    }
  }
  return best;
}

static int build_path(process *p, uint64 file_idx, char *out, int out_sz) {
  if (!p || !p->file || !out || out_sz <= 0) return -1;
  if (file_idx >= 64) return -1;

  const char *fname = p->file[file_idx].file;
  if (!fname || fname[0] == 0) return -1;

  fname = normalize_src_path(fname);

  if (strchr(fname, '/')) {
    safestrcpy(out, fname, out_sz);
    return 0;
  }

  const char *dname = 0;
  uint64 dir_idx = p->file[file_idx].dir;
  if (p->dir && dir_idx < 64) dname = p->dir[dir_idx];
  if (dname) dname = normalize_src_path(dname);

  if (!dname || dname[0] == 0) {
    safestrcpy(out, fname, out_sz);
    return 0;
  }

  int n = 0;
  for (; n < out_sz - 1 && dname[n]; n++) out[n] = dname[n];
  if (n > 0 && out[n - 1] != '/' && n < out_sz - 1) out[n++] = '/';
  for (int i = 0; n < out_sz - 1 && fname[i]; i++) out[n++] = fname[i];
  out[n] = 0;
  return 0;
}
static int read_source_line(const char *path, uint64 lineno, char *out, int out_sz) {
  if (!path || !out || out_sz <= 0 || lineno == 0) return -1;
  out[0] = 0;

  spike_file_t *f = spike_file_open(path, O_RDONLY, 0);
  if (IS_ERR_VALUE(f)) return -1;

  uint64 cur = 1;
  int idx = 0;
  char buf[128];

  for (;;) {
    ssize_t r = spike_file_read(f, buf, sizeof(buf));
    if (r <= 0) break;

    for (ssize_t i = 0; i < r; i++) {
      char c = buf[i];

      if (cur == lineno) {
        if (c == '\n' || c == '\r') {
          spike_file_close(f);
          out[idx] = 0;
          return 0;
        }
        if (idx < out_sz - 1) out[idx++] = c;
      }

      if (c == '\n') {
        cur++;
        if (cur > lineno) {
          spike_file_close(f);
          out[idx] = 0;
          return 0;
        }
      }
    }
  }

  spike_file_close(f);
  out[idx] = 0;
  return (idx > 0) ? 0 : -1;
}

static char *ltrim(char *s) {
  if (!s) return s;
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

void print_errorline(uint64 fault_pc) {
  if (!current || !current->debugline || current->line_ind <= 0) return;

  addr_line *al = lookup_addr_line(current, fault_pc);
  if (!al) return;
  if (!current->file) return;

  char path[256];
  if (build_path(current, al->file, path, sizeof(path)) != 0) return;

  sprint("Runtime error at %s:%ld\n", path, (long)al->line);

  char linebuf[256];
  if (read_source_line(path, al->line, linebuf, sizeof(linebuf)) == 0) {
    sprint("  %s\n", ltrim(linebuf));
  }
}
















