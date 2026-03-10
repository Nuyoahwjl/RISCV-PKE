#include "kernel/riscv.h"
#include "kernel/process.h"
#include "kernel/elf.h"
#include "spike_interface/spike_utils.h"

static void handle_instruction_access_fault() {
  print_errorline(read_csr(mepc));
  panic("Instruction access fault!");
}

static void handle_load_access_fault() {
  print_errorline(read_csr(mepc));
  panic("Load access fault!");
}

static void handle_store_access_fault() {
  print_errorline(read_csr(mepc));
  panic("Store/AMO access fault!");
}

static void handle_illegal_instruction() {
  print_errorline(read_csr(mepc));
  panic("Illegal instruction!");
}

static void handle_misaligned_load() {
  print_errorline(read_csr(mepc));
  panic("Misaligned Load!");
}

static void handle_misaligned_store() {
  print_errorline(read_csr(mepc));
  panic("Misaligned AMO!");
}

static void handle_timer() {
  int cpuid = 0;
  *(uint64 *)CLINT_MTIMECMP(cpuid) = *(uint64 *)CLINT_MTIMECMP(cpuid) + TIMER_INTERVAL;
  write_csr(sip, SIP_SSIP);
}

void handle_mtrap() {
  uint64 mcause = read_csr(mcause);
  switch (mcause) {
    case CAUSE_MTIMER:
      handle_timer();
      break;
    case CAUSE_FETCH_ACCESS:
      handle_instruction_access_fault();
      break;
    case CAUSE_LOAD_ACCESS:
      handle_load_access_fault();
      break;
    case CAUSE_STORE_ACCESS:
      handle_store_access_fault();
      break;
    case CAUSE_ILLEGAL_INSTRUCTION:
      handle_illegal_instruction();
      break;
    case CAUSE_MISALIGNED_LOAD:
      handle_misaligned_load();
      break;
    case CAUSE_MISALIGNED_STORE:
      handle_misaligned_store();
      break;
    default:
      sprint("machine trap(): unexpected mscause %p\n", mcause);
      sprint("            mepc=%p mtval=%p\n", read_csr(mepc), read_csr(mtval));
      print_errorline(read_csr(mepc));
      panic("unexpected exception happened in M-mode.\n");
      break;
  }
}
