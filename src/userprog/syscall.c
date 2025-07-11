#include "userprog/syscall.h"

#include <hash.h>
#include <stdio.h>
#include <syscall-nr.h>

#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/frame.h"
#include "vm/mmap.h"
#include "vm/page.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}

void exit(int status) {
  printf("%s: exit(%d)\n", thread_name(), status);
  thread_current()->exit_status = status;
  thread_exit();
}

int open(const char* file) {
  struct file** fd_table = thread_current()->fd_table;
  lock_acquire(&filesys_lock);
  struct file* f = filesys_open(file);
  if (f == NULL) {
    lock_release(&filesys_lock);
    return -1;  // error
  }

  int i;
  for (i = 2; i < 130; i++) {
    if (fd_table[i] == NULL) {
      fd_table[i] = f;
      lock_release(&filesys_lock);
      return i;
    }
  }
  lock_release(&filesys_lock);
  return -1;
}

int filesize(int fd) {
  struct file** fd_table = thread_current()->fd_table;
  if (fd < 2 || fd >= FD_TABLE_SIZE || fd_table[fd] == NULL) {
    exit(-1);
    return -1;
  }
  struct file* f = fd_table[fd];
  if (f == NULL) {
    return -1;  // error
  }
  lock_acquire(&filesys_lock);
  int ret = file_length(f);
  lock_release(&filesys_lock);
  return ret;
}

int read(int fd, void* buffer, unsigned size) {
  if (fd < 0 || fd == 1 || fd >= FD_TABLE_SIZE) {
    exit(-1);
    return -1;
  }
  int j;
  for (j = 0; j < size; j++) touch_addr(buffer + j);

  lock_acquire(&filesys_lock);
  if (fd == 0) {
    int i;
    int* buffer_c = buffer;
    for (i = 0; i < size; i++) buffer_c[i] = input_getc();
    lock_release(&filesys_lock);
    return size;
  }

  struct file** fd_table = thread_current()->fd_table;
  struct file* f = fd_table[fd];
  if (f == NULL) {
    lock_release(&filesys_lock);
    return -1;  // error
  }
  int ret = file_read(f, buffer, size);
  lock_release(&filesys_lock);
  return ret;
}

int write(int fd, void* buffer, unsigned size) {
  if (fd < 1 || fd >= FD_TABLE_SIZE) {
    exit(-1);
    return -1;
  }
  int j;
  for (j = 0; j < size; j++) touch_addr(buffer + j);

  lock_acquire(&filesys_lock);
  if (fd == 1) {
    putbuf(buffer, size);
    lock_release(&filesys_lock);
    return size;
  } else {
    struct file** fd_table = thread_current()->fd_table;
    struct file* f = fd_table[fd];
    if (f == NULL) {
      lock_release(&filesys_lock);
      return -1;  // error
    }
    int ret = file_write(f, buffer, size);
    lock_release(&filesys_lock);
    return ret;
  }
  lock_release(&filesys_lock);
  return -1;
}

void seek(int fd, unsigned position) {
  struct file** fd_table = thread_current()->fd_table;
  if (fd < 2 || fd >= FD_TABLE_SIZE || fd_table[fd] == NULL) {
    exit(-1);
    return -1;
  }
  lock_acquire(&filesys_lock);
  struct file* f = fd_table[fd];
  file_seek(f, position);
  lock_release(&filesys_lock);
}

unsigned tell(int fd) {
  struct file** fd_table = thread_current()->fd_table;
  if (fd < 2 || fd >= FD_TABLE_SIZE || fd_table[fd] == NULL) {
    exit(-1);
    return -1;
  }
  lock_acquire(&filesys_lock);
  struct file* f = fd_table[fd];
  unsigned ret = file_tell(f);
  lock_release(&filesys_lock);
  return ret;
}

void close(int fd) {
  struct file** fd_table = thread_current()->fd_table;
  if (fd < 2 || fd >= FD_TABLE_SIZE || fd_table[fd] == NULL) {
    exit(-1);
    return;
  } else {
    lock_acquire(&filesys_lock);
    file_close(fd_table[fd]);
    fd_table[fd] = NULL;
    lock_release(&filesys_lock);
    return;
  }
}

/* A function which checks if a virtual address is valid.
   If not, the thread exits with exit code -1 */
void check_valid(void* addr) {
  uint32_t* current_pd = thread_current()->pagedir;
  if (!addr || !is_user_vaddr(addr) || !pagedir_get_page(current_pd, addr)) {
    exit(-1);
    return;
  }
}

/* Cause page fault by "touching". This forces lazy-load to actually load
 * something in that page.*/
void touch_addr(void* addr) {
  uint8_t* temp_addr = (void*)addr;
  volatile uint8_t touch = *temp_addr;
}

/* Map files into process address space */
int mmap(int fd, void* addr) {
  // Validation
  if (fd == 0 || fd == 1 || addr == NULL) return -1;
  if (pg_ofs(addr) != 0) return -1;
  struct thread* t = thread_current();
  struct file** fd_table = t->fd_table;
  struct file* f = fd_table[fd];
  if (f == NULL) return -1;
  off_t len = file_length(f);
  if (len == 0) return -1;
  // The range of pages mapped overlaps any exisitng set of mapped pages -> fail
  if (find_mapping_addr(&t->mmap_table, addr) != NULL) return -1;
  if (addr >= PHYS_BASE - PGSIZE || addr <= t->data_segment_start) return -1;

  // Insert mapping to mmap_table
  struct mapping* m = malloc(sizeof(struct mapping));
  m->id = list_size(&t->mmap_table) + 1;
  m->addr = addr;
  m->size = len;
  m->file = file_reopen(f);
  m->fd = fd;
  list_init(&m->pages);
  list_push_back(&t->mmap_table, &m->elem);

  // Page-wise file mapping
  off_t read_bytes = len;
  off_t ofs = 0;
  while (read_bytes > 0) {
    off_t page_read_bytes = PGSIZE, page_zero_bytes;
    if (read_bytes < PGSIZE) page_read_bytes = read_bytes;
    page_zero_bytes = PGSIZE - page_read_bytes;

    uint8_t* kpage = palloc_get_page(0);
    struct page *temp = SPT_insert(m->file, ofs, addr, kpage, page_read_bytes, page_zero_bytes,
               true, FOR_MMAP);
    list_push_back(&m->pages, &temp->MMAP_elem);

    if (file_read(m->file, kpage, page_read_bytes) != page_read_bytes) {
      ASSERT(0);
      palloc_free_page(kpage);
      return -1;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    read_bytes -= page_read_bytes;
    addr += PGSIZE;
    ofs += page_read_bytes;
  }

  // return mapping id
  return m->id;
}

/* Write mapping's content to the file */
void munmap_write(struct thread* t, int mapping, bool unmap) {
  struct mapping* m = find_mapping_id(&t->mmap_table, mapping);
  if (m == NULL) exit(-1);

  // Check whether the pages are dirty. If so, call `file_write_at`
  struct list_elem* e;
  // struct frame* f;
  struct hash* spt = &t->SPT;
  struct hash_iterator it;
  hash_first(&it, spt);
  lock_acquire(&filesys_lock);
  while (hash_next(&it)) {
    struct page* p = hash_entry(hash_cur(&it), struct page, SPT_elem);
    if (p->purpose != FOR_MMAP) continue;
    void* addr = p->page_addr;
    if (pagedir_is_dirty(t->pagedir, addr))
      file_write_at(p->page_file, p->page_addr, p->read_bytes, p->ofs);
  }
  lock_release(&filesys_lock);
}

void munmap_free(struct thread* t, int mapping) {
  struct mapping* m = find_mapping_id(&t->mmap_table, mapping);

  if (m == NULL) {
    exit(-1);
  }

  // Close reopened file
  // file_close(m->file);

  // free mapping with unmapping page, clearing spt, free frame entry, ...
  struct list_elem* e;
  for (e = list_begin(&m->pages); e != list_end(&m->pages); e = list_next(e)) {
    struct page* p = list_entry(e, struct page, MMAP_elem);
    frame_free(p->page_addr);
    pagedir_clear_page(t->pagedir, p->page_addr);
    // SPT_remove(p->page_addr);
    hash_delete(&t->SPT, &p->SPT_elem);
  }
  list_remove(&m->elem);
  free(m);

  /*
  // Check whether the pages are dirty. If so, call `file_write_at`
  struct list_elem* e;
  // struct frame* f;
  struct hash* spt = &t->SPT;
  struct hash_iterator it;

  // free mapping with unmapping page, clearing spt, free frame entry, ...

  hash_first(&it, spt);
  while (hash_next(&it)) {
    struct page* p = hash_entry(hash_cur(&it), struct page, SPT_elem);
    if (p->purpose != FOR_MMAP) continue;
    void* addr = p->page_addr;
    void* frame_addr = p->frame_addr;
    // frame_free(addr);
    // pagedir_clear_page(t->pagedir, addr);
    // palloc_free_page(frame_addr);
  }

  // hash_delete(&t->SPT, &m->elem);
  // free(m);
  */
}

/* Unmap the mapping */
void munmap(int mapping) {
  /*
  struct thread* t = thread_current();
  struct mapping* m = find_mapping_id(&t->mmap_table, mapping);
  if (m == NULL) exit(-1);

  // Check whether the pages are dirty. If so, call `file_write_at`
  struct list_elem* e;
  // struct frame* f;
  struct hash* spt = &t->SPT;
  struct hash_iterator it;
  hash_first(&it, spt);
  lock_acquire(&filesys_lock);
  while (hash_next(&it)) {
    struct page* p = hash_entry(hash_cur(&it), struct page, SPT_elem);
    if (p->purpose != FOR_MMAP) continue;
    void* addr = p->page_addr;
    if (pagedir_is_dirty(t->pagedir, addr))
      file_write_at(p->page_file, p->page_addr, p->read_bytes, p->ofs);
  }
  lock_release(&filesys_lock);

  // Close reopened file
  file_close(m->file);

  // free mapping with unmapping page, clearing spt, free frame entry, ...
  hash_first(&it, spt);
  while (hash_next(&it)) {
    struct page* p = hash_entry(hash_cur(&it), struct page, SPT_elem);
    if (p->purpose != FOR_MMAP) continue;
    void* addr = p->page_addr;
    void* frame_addr = p->frame_addr;
    frame_free(addr);
    pagedir_clear_page(t->pagedir, addr);
    palloc_free_page(frame_addr);
  }

  hash_delete(&t->SPT, &m->elem);
  free(m);
  */

  struct thread* t = thread_current();
  munmap_write(t, mapping, false);
  munmap_free(t, mapping);
}

static void syscall_handler(struct intr_frame* f) {
  // printf("case: %d\n", *(uint32_t*)f->esp);

  // Before handling system call:
  // Check if the stack pointer is valid (sc-bad-sp)
  check_valid(f->esp);

  // handling system call
  switch (*(uint32_t*)f->esp) {
    case SYS_HALT:
      shutdown_power_off();
      break;

    case SYS_EXIT:
      check_valid(f->esp + 4);
      f->eax = *(uint32_t*)(f->esp + 4);  // update return value
      exit(*(uint32_t*)(f->esp + 4));
      break;

    case SYS_EXEC:
      // pid_t exec (const char *cmd_line)

      // Runs the executable whose name is given in cmd line, passing any given
      // arguments, and returns the new process’s program id(pid).

      // tid_t process_execute(const char* file_name) // in process.c

      check_valid(f->esp + 4);

      if (!pagedir_get_page((uint32_t*)thread_current()->pagedir,
                            (const void*)*(uint32_t*)(f->esp + 4))) {
        f->eax = -1;
        exit(-1);
        return;
      }

      tid_t pid;
      lock_acquire(&filesys_lock);
      pid = process_execute((const void*)*(uint32_t*)(f->esp + 4));

      // Search point to thread created, then wait for its loading
      // If the loading failed, return value should become -1
      struct list_elem* e;
      for (e = list_begin(&(thread_current()->children));
           e != list_end(&(thread_current()->children)); e = list_next(e)) {
        struct thread* t = list_entry(e, struct thread, childelem);
        if (t->tid == pid) {
          sema_down(&(t->load_sema));
          if (!t->load_status) pid = -1;
          break;
        }
      }

      lock_release(&filesys_lock);
      f->eax = pid;
      break;

    case SYS_WAIT:
      // int wait (pid t pid)

      // Waits for a child process pid and retrieves the child’s exit status.
      // Implementing this system call requires considerably more work than any
      // of the rest.

      // int process_wait(tid_t child_tid) // in process.c

      check_valid(f->esp + 4);
      f->eax = process_wait((tid_t) * (uint32_t*)(f->esp + 4));
      break;

    case SYS_CREATE:
      // bool create (const char *file, unsigned initial_size)

      // Creates a new file called file initially initial size bytes in size.
      // Returns true if successful, false otherwise.

      check_valid(f->esp + 4);
      check_valid(f->esp + 8);

      if (!is_user_vaddr((void*)*(uint32_t*)(f->esp + 4)) ||
          !pagedir_get_page((uint32_t*)thread_current()->pagedir,
                            (const void*)*(uint32_t*)(f->esp + 4))) {
        f->eax = -1;  // return 0 (false)
        exit(-1);
        return;
      }

      if (filesys_create((const char*)*(uint32_t*)(f->esp + 4),
                         (unsigned)*((uint32_t*)(f->esp + 8)))) {
        f->eax = 1;  // return 1 (true)
      } else {
        f->eax = 0;  // return 0 (false)
      }
      break;

    case SYS_REMOVE:
      // bool remove (const char *file)

      // Deletes the file called file. Returns true if successful, false
      // otherwise. A file may be removed regardless of whether it is open or
      // closed, and removing an open file does not close it.

      // bool filesys_remove(const char* name) // in filesys.c

      check_valid(f->esp + 4);

      if (!is_user_vaddr((void*)*(uint32_t*)(f->esp + 4)) ||
          !pagedir_get_page((uint32_t*)thread_current()->pagedir,
                            (const void*)*(uint32_t*)(f->esp + 4))) {
        f->eax = -1;  // return -1 (error)
        exit(-1);
        return;
      }

      f->eax = filesys_remove((const char*)*(uint32_t*)(f->esp + 4));

      break;

    case SYS_OPEN:
      // int open (const char *file)

      // Opens the file called file. Returns a nonnegative integer handle called
      // a “file descriptor” (fd), or -1 if the file could not be opened.

      check_valid(f->esp + 4);

      if (!is_user_vaddr((void*)*(uint32_t*)(f->esp + 4)) ||
          !pagedir_get_page((uint32_t*)thread_current()->pagedir,
                            (const void*)*(uint32_t*)(f->esp + 4))) {
        f->eax = -1;  // return -1 (error)
        exit(-1);
        return;
      }

      f->eax = open((const char*)*(uint32_t*)(f->esp + 4));
      break;

    case SYS_FILESIZE:
      // int filesize (int fd)

      // Returns the size, in bytes, of the file open as fd.

      // struct inode_disk has member: off_t length, which is file size in bytes
      check_valid(f->esp + 4);
      f->eax = filesize((int)*(uint32_t*)(f->esp + 4));

      break;

    case SYS_READ:
      // int read(int fd, void* buffer, unsigned size)

      // Reads size bytes from the file open as fd into buffer. Returns the
      // number of bytes actually read(0 at end of file), or -1 if the file
      // could not be read(due to a condition other than end of file).Fd 0 reads
      // from the keyboard using input_getc().

      // off_t file_read(struct file* file, void* buffer, off_t size) // in
      // file.c

      check_valid(f->esp + 4);
      check_valid(f->esp + 8);
      check_valid(f->esp + 12);

      touch_addr((const void*)*(uint32_t*)(f->esp + 8));

      if (!is_user_vaddr((void*)*(uint32_t*)(f->esp + 8)) ||
          !pagedir_get_page((uint32_t*)thread_current()->pagedir,
                            (const void*)*(uint32_t*)(f->esp + 8))) {
        f->eax = -1;  // return -1 (false)
        exit(-1);
        return;
      }

      f->eax =
          read((int)*(uint32_t*)(f->esp + 4), (void*)*(uint32_t*)(f->esp + 8),
               (unsigned)*((uint32_t*)(f->esp + 12)));

      break;

    case SYS_WRITE:
      // int write (int fd, const void *buffer, unsigned size)

      // Writes size bytes from buffer to the open file fd. Returns the number
      // of bytes actually written, which may be less than size if some bytes
      // could not be written.

      // off_t file_write(struct file* file, const void* buffer, off_t size) //
      // in file.c

      check_valid(f->esp + 4);
      check_valid(f->esp + 8);
      check_valid(f->esp + 12);

      touch_addr((const void*)*(uint32_t*)(f->esp + 8));

      if (!is_user_vaddr((void*)*(uint32_t*)(f->esp + 8)) ||
          !pagedir_get_page((uint32_t*)thread_current()->pagedir,
                            (const void*)*(uint32_t*)(f->esp + 8))) {
        f->eax = -1;  // return -1 (false)
        exit(-1);
        return;
      }

      f->eax =
          write((int)*(uint32_t*)(f->esp + 4), (void*)*(uint32_t*)(f->esp + 8),
                (unsigned)*((uint32_t*)(f->esp + 12)));
      break;

    case SYS_SEEK:
      // void seek (int fd, unsigned position)

      // Changes the next byte to be read or written in open file fd to
      // position, expressed in bytes from the beginning of the file.

      // void file_seek(struct file* file, off_t new_pos) // in file.c

      check_valid(f->esp + 4);
      check_valid(f->esp + 8);

      seek((int)*(uint32_t*)(f->esp + 4), (unsigned)*((uint32_t*)(f->esp + 8)));

      break;

    case SYS_TELL:
      // unsigned tell (int fd)

      // Returns the position of the next byte to be read or written in open
      // file fd, expressed in bytes from the beginning of the file.

      // off_t file_tell(struct file* file) // in file.c

      check_valid(f->esp + 4);
      check_valid(f->esp + 8);

      f->eax = tell((int)*(uint32_t*)(f->esp + 4));

      break;

    case SYS_CLOSE:
      check_valid(f->esp + 4);
      close((int)*(uint32_t*)(f->esp + 4));
      break;

    case SYS_MMAP: /* Map a file into memory. */
      // mapid_t mmap(int fd, void *addr)

      check_valid(f->esp + 4);
      check_valid(f->esp + 8);

      f->eax =
          mmap((int)*(uint32_t*)(f->esp + 4), (void*)*(uint32_t*)(f->esp + 8));

      break;

    case SYS_MUNMAP: /* Remove a memory mapping. */
      // void munmap(mapid_t mapping);
      check_valid(f->esp + 4);

      munmap((int)*(uint32_t*)(f->esp + 4));

      break;

    default:
      break;
  }
}
