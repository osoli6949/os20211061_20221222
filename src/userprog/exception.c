#include "userprog/exception.h"

#include <debug.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/off_t.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"  // will be needed for stack swap!

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill(struct intr_frame*);
static void page_fault(struct intr_frame*);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void exception_init(void) {
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int(3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int(4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int(5, 3, INTR_ON, kill, "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int(0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int(1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int(6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int(7, 0, INTR_ON, kill, "#NM Device Not Available Exception");
  intr_register_int(11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int(12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int(13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int(16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int(19, 0, INTR_ON, kill, "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int(14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void exception_print_stats(void) {
  printf("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void kill(struct intr_frame* f) {
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */

  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs) {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf("%s: dying due to interrupt %#04x (%s).\n", thread_name(),
             f->vec_no, intr_name(f->vec_no));
      intr_dump_frame(f);
      thread_exit();

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame(f);
      PANIC("Kernel bug - unexpected interrupt in kernel");

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf("Interrupt %#04x (%s) in unknown segment %04x\n", f->vec_no,
             intr_name(f->vec_no), f->cs);
      thread_exit();
  }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void page_fault(struct intr_frame* f) {
  bool not_present; /* True: not-present page, false: writing r/o page. */
  bool write;       /* True: access was write, false: access was read. */
  bool user;        /* True: access by user, false: access by kernel. */
  void* fault_addr; /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm("movl %%cr2, %0" : "=r"(fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* To implement virtual memory, delete the rest of the function
     body, and replace it with code that brings in the page to
     which fault_addr refers. */

  //  #ifdef USERPROG
  //    printf("%s: exit(%d)\n", thread_name(),
  //           thread_current()->exit_status);  // FIXME: pseudo-calling exit()
  //  #endif
  //    kill(f);

  // #ifdef USERPROG
  //  struct thread* cur = thread_current();
  //  if (fault_addr == NULL || !is_user_vaddr(fault_addr)) exit(-1);
  // #endif

  void* esp;
  if (user)
    esp = f->esp;
  else
    esp = thread_current()->esp;

  // Case 0. Bad access -> just raise error.
  if (fault_addr == NULL || !is_user_vaddr(fault_addr)) {
    exit(-1);
  }

  // printf("Fault at %p, eip = %p, esp = %p\n", fault_addr, f->eip, f->esp);

  ASSERT(is_user_vaddr(fault_addr));

  void* fault_page_addr = pg_round_down(fault_addr);
  // printf("Search for %p\n", fault_page_addr);
  struct page* fault_page = SPT_search(thread_current(), fault_page_addr);

  // Case 1. SPT does not exist
  //  -> page fault is caused by stack growth attempt.
  if (!fault_page) {
    // 8MB stack size limit.
    if (fault_addr <= PHYS_BASE - 0x800000) exit(-1);

    if (fault_addr >= esp - 32) {
      void* kpage = frame_alloc(PAL_USER | PAL_ZERO, false);
      struct frame* f = find_frame(kpage);
      f->is_evictable = true;
      f->owner_thread = thread_current();

      pagedir_set_page(thread_current()->pagedir, fault_page_addr, kpage, true);
      SPT_insert(NULL, 0, fault_page_addr, kpage, 0, PGSIZE, true, FOR_STACK);
      thread_current()->esp = fault_addr;
      return;
    } else {
      // printf("STACK GROWTH ERROR\n");
      exit(-1);
    }
  }

  // Case 2. SPT does exist
  else {
    // Reload load_segment's arguments
    struct file* file = fault_page->page_file;
    off_t ofs = fault_page->ofs;
    uint8_t* upage = fault_page->page_addr;
    size_t page_read_bytes = fault_page->read_bytes;
    size_t page_zero_bytes = fault_page->zero_bytes;
    bool writable = fault_page->is_writable;

    // If this fault is caused by write, but the page is not writable,
    // raise error!
    if (write && !writable) {
      // printf("WRITE PERM ERROR\n");
      exit(-1);
    }
    switch (fault_page->purpose) {
      case FOR_FILE:
        if (!fault_page->is_swapped) {
          // Repeat load_segment
          file_seek(file, ofs);
          uint8_t* kpage = frame_alloc(PAL_USER, true);

          struct frame* f = find_frame(kpage);
          f->page_addr = upage;
          f->is_evictable = true;
          f->owner_thread = thread_current();

          fault_page->frame_addr = kpage;
          fault_page->is_swapped = false;

          off_t n = file_read(file, kpage, page_read_bytes);
          if (n != (int)page_read_bytes) {
            // printf("File read error\n");
            frame_free(kpage);
            exit(-1);
          }
          memset(kpage + page_read_bytes, 0, page_zero_bytes);

          bool ok = pagedir_set_page(thread_current()->pagedir, upage, kpage,
                                     writable);
          if (!ok) {
            printf("Failed!: pagedir_set_page in thread: %s\n", thread_name());
          }

          return;

        } else {
          // FIXME: Page is in the swap disk.
          size_t swap_i = fault_page->swap_i;

          // Repeat load_segment
          file_seek(file, ofs);
          uint8_t* kpage = frame_alloc(PAL_USER, true);
          // if (!kpage) printf("Your frame_alloc is trash\n");
          struct frame* f = find_frame(kpage);
          f->page_addr = upage;
          f->is_evictable = true;
          f->owner_thread = thread_current();

          fault_page->frame_addr = kpage;

          // Read from corresponding disk file.
          /*
          printf(
              "SPT_search hit: %p → frame_addr = %p, is_swapped = %d, swap_i = "
              "%zu\n",
              fault_page_addr, fault_page->frame_addr, fault_page->is_swapped,
              fault_page->swap_i);
              */

          SD_read(swap_i, kpage);
          fault_page->is_swapped = false;
          // memset(kpage + page_read_bytes, 0, page_zero_bytes);
          bool ok = pagedir_set_page(thread_current()->pagedir, upage, kpage,
                                     writable);
          if (!ok) {
            printf("Failed!: pagedir_set_page in thread: %s\n", thread_name());
          }
          return;
        }

        break;

      case FOR_STACK:
        if (!fault_page->is_swapped) {
          // This case should've covered by !fault_page check.
          // I have no idea. Let's just pray.

          // Allocate frame.
          uint8_t* kpage = frame_alloc(PAL_USER, false);
          fault_page->frame_addr = kpage;

          // Setup stack.
          pagedir_set_page(thread_current()->pagedir, upage, kpage, writable);
          thread_current()->esp = fault_addr;
          return;

        } else {
          // Page is in the swap disk.
          size_t swap_i = fault_page->swap_i;

          // Allocate frame
          uint8_t* kpage = frame_alloc(PAL_USER, false);
          struct frame* f = find_frame(kpage);
          f->page_addr = upage;
          f->is_evictable = true;
          f->owner_thread = thread_current();

          fault_page->frame_addr = kpage;
          fault_page->is_swapped = false;

          // Read from corresponding disk file.
          // printf("Stack reading swap disk\n");
          SD_read(swap_i, kpage);

          pagedir_set_page(thread_current()->pagedir, upage, kpage, writable);
          thread_current()->esp = fault_addr;
          return;
        }
        break;

      case FOR_MMAP:
        /* TEMPORARILY COPIED FROM `FOR_FILE` */
        if (!fault_page->is_swapped) {
          // Repeat load_segment
          file_seek(file, ofs);
          uint8_t* kpage = frame_alloc(PAL_USER, true);

          struct frame* f = find_frame(kpage);
          f->page_addr = upage;
          f->is_evictable = true;
          f->owner_thread = thread_current();

          fault_page->frame_addr = kpage;
          fault_page->is_swapped = false;

          off_t n = file_read(file, kpage, page_read_bytes);
          if (n != (int)page_read_bytes) {
            // printf("File read error\n");
            frame_free(kpage);
            exit(-1);
          }
          memset(kpage + page_read_bytes, 0, page_zero_bytes);

          bool ok = pagedir_set_page(thread_current()->pagedir, upage, kpage,
                                     writable);
          if (!ok) {
            printf("Failed!: pagedir_set_page in thread: %s\n", thread_name());
          }

          return;

        } else {
          // FIXME: Page is in the swap disk.
          size_t swap_i = fault_page->swap_i;

          // Repeat load_segment
          file_seek(file, ofs);
          uint8_t* kpage = frame_alloc(PAL_USER, true);
          // if (!kpage) printf("Your frame_alloc is trash\n");
          struct frame* f = find_frame(kpage);
          f->page_addr = upage;
          f->is_evictable = true;
          f->owner_thread = thread_current();

          fault_page->frame_addr = kpage;

          // Read from corresponding disk file.
          /*
          printf(
              "SPT_search hit: %p → frame_addr = %p, is_swapped = %d, swap_i = "
              "%zu\n",
              fault_page_addr, fault_page->frame_addr, fault_page->is_swapped,
              fault_page->swap_i);
              */

          SD_read(swap_i, kpage);
          fault_page->is_swapped = false;
          // memset(kpage + page_read_bytes, 0, page_zero_bytes);
          bool ok = pagedir_set_page(thread_current()->pagedir, upage, kpage,
                                     writable);
          if (!ok) {
            printf("Failed!: pagedir_set_page in thread: %s\n", thread_name());
          }
          return;
        }
        break;

      default:
        printf("You reached the undefined purpose\n");
        exit(-1);
    }
  }

  printf("You reached the unreachable.\n");

  exit(-1);
}
