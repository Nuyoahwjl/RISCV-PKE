/*
 * routines that scan and load a (host) Executable and Linkable Format (ELF) file
 * into the (emulated) memory.
 */

#include "elf.h"
#include "string.h"
#include "riscv.h"
#include "process.h"
#include "spike_interface/spike_file.h"
#include "spike_interface/spike_utils.h"

typedef struct elf_info_t {
  spike_file_t *f;
  process *p;
} elf_info;

//
// the implementation of allocater. allocates memory space for later segment loading
//
static void *elf_alloc_mb(elf_ctx *ctx, uint64 elf_pa, uint64 elf_va, uint64 size) {
  // directly returns the virtual address as we are in the Bare mode in lab1_x
  return (void *)elf_va;
}

//
// actual file reading, using the spike file interface.
//
static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset) {
  elf_info *msg = (elf_info *)ctx->info;
  // call spike file utility to load the content of elf file into memory.
  // spike_file_pread will read the elf file (msg->f) from offset to memory (indicated by
  // *dest) for nb bytes.
  return spike_file_pread(msg->f, dest, nb, offset);
}

//
// init elf_ctx, a data structure that loads the elf.
//
elf_status elf_init(elf_ctx *ctx, void *info) {
  ctx->info = info;

  // load the elf header
  if (elf_fpread(ctx, &ctx->ehdr, sizeof(ctx->ehdr), 0) != sizeof(ctx->ehdr)) return EL_EIO;

  // check the signature (magic value) of the elf
  if (ctx->ehdr.magic != ELF_MAGIC) return EL_NOTELF;

  return EL_OK;
}

// leb128 (little-endian base 128) is a variable-length
// compression algoritm in DWARF
void read_uleb128(uint64 *out, char **off) {
    uint64 value = 0; int shift = 0; uint8 b;
    for (;;) {
        b = *(uint8 *)(*off); (*off)++;
        value |= ((uint64)b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
    }
    if (out) *out = value;
}
void read_sleb128(int64 *out, char **off) {
    int64 value = 0; int shift = 0; uint8 b;
    for (;;) {
        b = *(uint8 *)(*off); (*off)++;
        value |= ((uint64_t)b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
    }
    if (shift < 64 && (b & 0x40)) value |= -(1 << shift);
    if (out) *out = value;
}
// Since reading below types through pointer cast requires aligned address,
// so we can only read them byte by byte
void read_uint64(uint64 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 8; i++) {
        *out |= (uint64)(**off) << (i << 3); (*off)++;
    }
}
void read_uint32(uint32 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 4; i++) {
        *out |= (uint32)(**off) << (i << 3); (*off)++;
    }
}
void read_uint16(uint16 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 2; i++) {
        *out |= (uint16)(**off) << (i << 3); (*off)++;
    }
}

/*
* analyzis the data in the debug_line section
*
* the function needs 3 parameters: elf context, data in the debug_line section
* and length of debug_line section
*
* make 3 arrays:
* "process->dir" stores all directory paths of code files
* "process->file" stores all code file names of code files and their directory path index of array "dir"
* "process->line" stores all relationships map instruction addresses to code line numbers
* and their code file name index of array "file"
*/
void make_addr_line(elf_ctx *ctx, char *debug_line, uint64 length) {
   process *p = ((elf_info *)ctx->info)->p;
    p->debugline = debug_line;
    // directory name char pointer array
    p->dir = (char **)((((uint64)debug_line + length + 7) >> 3) << 3); int dir_ind = 0, dir_base;
    // file name char pointer array
    p->file = (code_file *)(p->dir + 64); int file_ind = 0, file_base;
    // table array
    p->line = (addr_line *)(p->file + 64); p->line_ind = 0;
    char *off = debug_line;
    while (off < debug_line + length) { // iterate each compilation unit(CU)
        debug_header *dh = (debug_header *)off; off += sizeof(debug_header);
        dir_base = dir_ind; file_base = file_ind;
        // get directory name char pointer in this CU
        while (*off != 0) {
            p->dir[dir_ind++] = off; while (*off != 0) off++; off++;
        }
        off++;
        // get file name char pointer in this CU
        while (*off != 0) {
            p->file[file_ind].file = off; while (*off != 0) off++; off++;
            uint64 dir; read_uleb128(&dir, &off);
            p->file[file_ind++].dir = dir - 1 + dir_base;
            read_uleb128(NULL, &off); read_uleb128(NULL, &off);
        }
        off++; addr_line regs; regs.addr = 0; regs.file = 1; regs.line = 1;
        // simulate the state machine op code
        for (;;) {
            uint8 op = *(off++);
            switch (op) {
                case 0: // Extended Opcodes
                    read_uleb128(NULL, &off); op = *(off++);
                    switch (op) {
                        case 1: // DW_LNE_end_sequence
                            if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                            p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                            p->line_ind++; goto endop;
                        case 2: // DW_LNE_set_address
                            read_uint64(&regs.addr, &off); break;
                        // ignore DW_LNE_define_file
                        case 4: // DW_LNE_set_discriminator
                            read_uleb128(NULL, &off); break;
                    }
                    break;
                case 1: // DW_LNS_copy
                    if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                    p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                    p->line_ind++; break;
                case 2: { // DW_LNS_advance_pc
                            uint64 delta; read_uleb128(&delta, &off);
                            regs.addr += delta * dh->min_instruction_length;
                            break;
                        }
                case 3: { // DW_LNS_advance_line
                            int64 delta; read_sleb128(&delta, &off);
                            regs.line += delta; break; } case 4: // DW_LNS_set_file
                        read_uleb128(&regs.file, &off); break;
                case 5: // DW_LNS_set_column
                        read_uleb128(NULL, &off); break;
                case 6: // DW_LNS_negate_stmt
                case 7: // DW_LNS_set_basic_block
                        break;
                case 8: { // DW_LNS_const_add_pc
                            int adjust = 255 - dh->opcode_base;
                            int delta = (adjust / dh->line_range) * dh->min_instruction_length;
                            regs.addr += delta; break;
                        }
                case 9: { // DW_LNS_fixed_advanced_pc
                            uint16 delta; read_uint16(&delta, &off);
                            regs.addr += delta;
                            break;
                        }
                        // ignore 10, 11 and 12
                default: { // Special Opcodes
                             int adjust = op - dh->opcode_base;
                             int addr_delta = (adjust / dh->line_range) * dh->min_instruction_length;
                             int line_delta = dh->line_base + (adjust % dh->line_range);
                             regs.addr += addr_delta;
                             regs.line += line_delta;
                             if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                             p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                             p->line_ind++; break;
                         }
            }
        }
endop:;
    }
    // for (int i = 0; i < p->line_ind; i++)
    //     sprint("%p %d %d\n", p->line[i].addr, p->line[i].line, p->line[i].file);
}

//
// load the elf segments to memory regions as we are in Bare mode in lab1
//
elf_status elf_load(elf_ctx *ctx) {
  // elf_prog_header structure is defined in kernel/elf.h
  elf_prog_header ph_addr;
  int i, off;

  // traverse the elf program segment headers
  for (i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum; i++, off += sizeof(ph_addr)) {
    // read segment headers
    if (elf_fpread(ctx, (void *)&ph_addr, sizeof(ph_addr), off) != sizeof(ph_addr)) return EL_EIO;

    if (ph_addr.type != ELF_PROG_LOAD) continue;
    if (ph_addr.memsz < ph_addr.filesz) return EL_ERR;
    if (ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr) return EL_ERR;

    // allocate memory block before elf loading
    void *dest = elf_alloc_mb(ctx, ph_addr.vaddr, ph_addr.vaddr, ph_addr.memsz);

    // actual loading
    if (elf_fpread(ctx, dest, ph_addr.memsz, ph_addr.off) != ph_addr.memsz)
      return EL_EIO;
  }

  return EL_OK;
}

typedef union {
  uint64 buf[MAX_CMDLINE_ARGS];
  char *argv[MAX_CMDLINE_ARGS];
} arg_buf;

// ---------------- lab1_challenge2: DWARF .debug_line loader ----------------
// Find a section by name (e.g. ".debug_line") and return its section header.
static int elf_find_section(elf_ctx *ctx, const char *secname, elf_sect_header *out) {
  // sanity: this lab uses the fixed 64-byte ELF64 section header.
  kassert(ctx->ehdr.shentsize == sizeof(elf_sect_header));

  // read the section-header-string-table (.shstrtab) header first
  elf_sect_header shstr;
  uint64 shstr_hdr_off = ctx->ehdr.shoff + (uint64)ctx->ehdr.shentsize * ctx->ehdr.shstrndx;
  if (elf_fpread(ctx, &shstr, sizeof(shstr), shstr_hdr_off) != sizeof(shstr)) return -1;

  // load .shstrtab into a small buffer (typically a few KB)
  enum { SHSTRTAB_MAX = 8192 };
  static char shstrtab[SHSTRTAB_MAX];
  if (shstr.size >= SHSTRTAB_MAX) return -1;
  if (elf_fpread(ctx, shstrtab, shstr.size, shstr.offset) != shstr.size) return -1;

  // traverse all section headers, compare names
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

//
// returns the number (should be 1) of string(s) after PKE kernel in command line.
// and store the string(s) in arg_bug_msg.
//
static size_t parse_args(arg_buf *arg_bug_msg) {
  // HTIFSYS_getmainvars frontend call reads command arguments to (input) *arg_bug_msg
  long r = frontend_syscall(HTIFSYS_getmainvars, (uint64)arg_bug_msg,
      sizeof(*arg_bug_msg), 0, 0, 0, 0, 0);
  kassert(r == 0);

  size_t pk_argc = arg_bug_msg->buf[0];
  uint64 *pk_argv = &arg_bug_msg->buf[1];

  int arg = 1;  // skip the PKE OS kernel string, leave behind only the application name
  for (size_t i = 0; arg + i < pk_argc; i++)
    arg_bug_msg->argv[i] = (char *)(uintptr_t)pk_argv[arg + i];

  //returns the number of strings after PKE kernel in command line
  return pk_argc - arg;
}

//
// load the elf of user application, by using the spike file interface.
//
void load_bincode_from_host_elf(process *p) {
  arg_buf arg_bug_msg;

  // retrieve command line arguements
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) panic("You need to specify the application program!\n");

  sprint("Application: %s\n", arg_bug_msg.argv[0]);

  //elf loading. elf_ctx is defined in kernel/elf.h, used to track the loading process.
  elf_ctx elfloader;
  // elf_info is defined above, used to tie the elf file and its corresponding process.
  elf_info info;

  info.f = spike_file_open(arg_bug_msg.argv[0], O_RDONLY, 0);
  info.p = p;
  // IS_ERR_VALUE is a macro defined in spike_interface/spike_htif.h
  if (IS_ERR_VALUE(info.f)) panic("Fail on openning the input application program.\n");

  // init elfloader context. elf_init() is defined above.
  if (elf_init(&elfloader, &info) != EL_OK)
    panic("fail to init elfloader.\n");

  // load elf. elf_load() is defined above.
  if (elf_load(&elfloader) != EL_OK) panic("Fail on loading elf.\n");

  // entry (virtual, also physical in lab1_x) address
  p->trapframe->epc = elfloader.ehdr.entry;

  // lab1_challenge2: load and parse DWARF .debug_line, build addr->(file,line) mapping.
  // This is used later to print source location when a runtime exception happens.
  elf_sect_header dbg;
  if (elf_find_section(&elfloader, ".debug_line", &dbg) == 0) {
    if (dbg.size > USER_DEBUG_INFO_SIZE) {
      panic(".debug_line section is too large: %llu bytes", (unsigned long long)dbg.size);
    }

    char *debug_buf = (char *)USER_DEBUG_INFO_BASE;
    if (elf_fpread(&elfloader, debug_buf, dbg.size, dbg.offset) != dbg.size) {
      panic("Fail on loading .debug_line section");
    }
    make_addr_line(&elfloader, debug_buf, dbg.size);
  } else {
    // no debug info available
    p->debugline = 0;
    p->dir = 0;
    p->file = 0;
    p->line = 0;
    p->line_ind = 0;
  }

  // close the host spike file
  spike_file_close( info.f );

  sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);
}



// Find the best addr_line entry for fault_pc.
// Policy: choose the entry with the largest addr <= fault_pc.
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
  if (!p || !p->file || !p->dir || !out || out_sz <= 0) return -1;
  if (file_idx >= 64) return -1;

  const char *fname = p->file[file_idx].file;
  uint64 dir_idx = p->file[file_idx].dir;
  const char *dname = (dir_idx < 64) ? p->dir[dir_idx] : 0;

  if (!fname) return -1;

  // If we don't have a directory, just use file name.
  //if (!dname || dname[0] == 0) {
  //  strcpy(out, fname, out_sz);
  //  return 0;
  //}

  // Join "dir/file".
  int n = 0;
  for (; n < out_sz - 1 && dname[n]; n++) out[n] = dname[n];
  if (n >= out_sz - 1) {
    out[out_sz - 1] = 0;
    return 0;
  }
  if (n > 0 && out[n - 1] != '/') out[n++] = '/';
  for (int i = 0; n < out_sz - 1 && fname[i]; i++) out[n++] = fname[i];
  out[n] = 0;
  return 0;
}

// Read a specific line (1-based) from a host file into out buffer.
// Returns 0 on success.
static int read_source_line(const char *path, uint64 lineno, char *out, int out_sz) {
  if (!path || !out || out_sz <= 0 || lineno == 0) return -1;
  out[0] = 0;

  spike_file_t *f = spike_file_open(path, O_RDONLY, 0);
  if (IS_ERR_VALUE(f)) return -1;

  uint64 cur = 1;
  int idx = 0;

  // Read in small chunks.
  char buf[128];
  for (;;) {
    ssize_t r = spike_file_read(f, buf, sizeof(buf));
    if (r <= 0) break;

    for (ssize_t i = 0; i < r; i++) {
      char c = buf[i];

      if (cur == lineno) {
        if (c == '\n' || c == '\r') {
          // End of the target line.
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
  if (!current->file || !current->dir) return;

  char path[256];
  if (build_path(current, al->file, path, sizeof(path)) != 0) return;

  sprint("Runtime error at %s:%ld\n", path, (long)al->line);

  // Print the actual source code line.
  char linebuf[256];
  if (read_source_line(path, al->line, linebuf, sizeof(linebuf)) == 0) {
    char *trimmed = ltrim(linebuf);
    sprint("  %s\n", trimmed);
  }
}
