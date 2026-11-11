# system calls

User space handlers for this. The kernel side of this is
`kernel/include/core/syscall/abi.hpp`; the handlers are in `kernel/src/core/syscall/`.

## Calling convention

A system call is a `SYSCALL` instruction.

| Register | Entry | Exit |
|---|---|---|
| RAX | Call number | Result, or `-error` |
| RDI, RSI, RDX, R10, R8, R9 | Arguments `a0` to `a5` | Kept, unless the call returns something there |
| R12, R13 | Message words 4 and 5 (IPC only) | Kept, unless the call returns a message |

A call that fails returns a negative value in RAX: the `Error` code, negated. A call that succeeds
never returns a negative value. The prototypes below give the type of a successful result; any of
them may return `-error` instead.

A number is a family and a function in it:

```
 63                 12 11      8 7            0
[    must be zero    |  family  |   function   ]
```

A number nothing answers to, including one with any bit set above the family, fails with
`-NotImplemented`.

Arguments that name a capability are a `cap_t` in the low 32 bits of the register. Where an
argument names a process, a thread or a CSpace, 0 means the caller's own.

## Types

```c
typedef uint32_t cap_t;   // A capability: slot index in bits 0-19, generation in bits 20-31.
                          // 0 is the null capability.
typedef uint32_t pid_t;   // A process ID. ~0 means any child, where a child is asked for.
typedef uint64_t tid_t;   // A thread ID.

// A message as it travels in registers. Where a prototype takes one, it stands for registers, not
// memory: the call reads it from them, and a call that receives one leaves it there, with the
// sender's badge in RDI.
struct message {
  uint64_t tag;       // RSI
  uint64_t words[6];  // RDX, R10, R8, R9, R12, R13
};

// The tag of a message.
//   bits  0-6   length: words in the message, 0 to 63. Words 6 and up are in the IPC buffer.
//   bits  7-9   extra_caps: capabilities sent along, 0 to 4, described in the IPC buffer.
//   bits 10-15  flags: 1 = notification bits, not a message; 4 = something went wrong on the way,
//               see status; 8 = the sender waits for a reply.
//   bits 16-31  status: the Error that went wrong on the way, if flag 4 is set.
//   bits 32-63  label: whatever sender and receiver agree on.

// What a long message, and capabilities, go through. Named with ipc_set_buffer().
struct cap_transfer {     // 16 bytes
  cap_t src_cap;          // In the sender's CSpace. 0 to send nothing in this place.
  uint8_t rights_mask;    // Rights the receiver gets, of those the sender has.
  uint8_t move;           // 1 to move it, 0 to copy it.
  uint16_t reserved;
  uint64_t new_badge;     // 0 to keep the badge it has.
};

struct ipc_buffer {                    // 576 bytes, aligned to 64
  uint64_t overflow_words[57];         // Message words 6 to 62.        offset 0
  struct cap_transfer send_caps[4];    // What to send along.           offset 456
  cap_t recv_caps[4];                  // What came, in this CSpace.    offset 520
  uint8_t received_cap_count;          // How many of them arrived.    offset 536
};

enum cap_type {             // For cap_create(). The others cannot be made from nothing.
  CAP_ENDPOINT = 1,
  CAP_NOTIFICATION = 2,
  CAP_VM_OBJECT = 4,
  CAP_CSPACE = 8,
};

enum cap_rights {           // A bit mask, 8 bits.
  RIGHT_READ = 0x01,        // Receive, wait, map readable.
  RIGHT_WRITE = 0x02,       // Send, call, signal, map writable.
  RIGHT_GRANT = 0x04,       // Send capabilities along.
  RIGHT_GRANT_REPLY = 0x08, // Let the reply to a call carry capabilities.
  RIGHT_MINT = 0x10,        // Badge a copy, or make a part of the object.
  RIGHT_REVOKE = 0x20,      // Revoke what was derived from an original.
  RIGHT_MANAGE = 0x40,      // Configure the object.
  RIGHT_EXECUTE = 0x80,     // Map executable.
  RIGHTS_ALL = 0xFF,
};

enum cap_flags {            // For cap_mint(), only CAP_PINNED may be asked for.
  CAP_PINNED = 0x04,        // Cannot be deleted or moved.
};

enum access_flags {         // A bit mask. Each call says which it takes.
  ACCESS_READ = 0x0001,
  ACCESS_WRITE = 0x0002,
  ACCESS_EXECUTE = 0x0004,
  ACCESS_STACK = 0x0800,    // Grows down when touched below its start.
  ACCESS_POPULATE = 0x1000, // Filled in at once, not on first touch.
};

enum page_size {
  PAGE_4K = 1,              // 0 means this too.
  PAGE_2M = 2,
  PAGE_1G = 3,
};

enum cache_mode {
  CACHE_WRITE_BACK = 0,
  CACHE_WRITE_THROUGH = 1,
  CACHE_UNCACHEABLE_MINUS = 2,
  CACHE_UNCACHEABLE = 3,
  CACHE_WRITE_COMBINING = 4,
  CACHE_WRITE_PROTECTED = 5,
};

struct dma_segment {        // 16 bytes
  uint64_t phys_addr;
  uint64_t size_bytes;
};

struct thread_stats {       // 48 bytes
  uint64_t switch_count;
  uint64_t voluntary_switches;
  uint64_t involuntary_switches;
  uint64_t total_runtime_ns;
  uint64_t last_switch_in_ns;
  uint64_t creation_time_ns;
};

struct memory_info {        // 40 bytes
  uint64_t total_bytes;     // RAM the kernel manages.
  uint64_t free_bytes;      // RAM free right now, about.
  uint64_t quota_used;      // What the caller's tree of processes is charged.
  uint64_t quota_limit;     // How far its quota goes.
  uint64_t quota_available; // What the caller can still be charged, right now.
};
```

## IPC (family `0x0`)

```c
/* 0x000 */ long  ipc_call(cap_t endpoint, struct message msg);
/* 0x001 */ long  ipc_reply_recv(cap_t endpoint, struct message reply);
/* 0x002 */ long  ipc_send(cap_t endpoint, struct message msg);
/* 0x003 */ long  ipc_try_send(cap_t endpoint, struct message msg);
/* 0x004 */ long  ipc_recv(cap_t endpoint);
/* 0x005 */ long  ipc_try_recv(cap_t endpoint);
/* 0x006 */ long  ipc_reply(cap_t saved_reply, struct message msg);
/* 0x007 */ cap_t ipc_save_reply(void);
/* 0x008 */ long  ipc_signal(cap_t notification);
/* 0x009 */ long  ipc_wait(cap_t notification);
/* 0x00A */ long  ipc_poll(cap_t notification);
/* 0x00B */ long  ipc_set_buffer(struct ipc_buffer* buffer);
```

- `ipc_call`, `ipc_reply_recv`, `ipc_recv` and `ipc_try_recv` return 0 and leave the message they
  received in the message registers, with its badge in RDI. `ipc_call` receives the reply;
  `ipc_reply_recv` answers the last call received, then waits for the next message.
- `ipc_try_send` and `ipc_try_recv` return `-WouldBlock` instead of waiting.
- `ipc_reply` answers the call saved as `saved_reply`, or with 0, the last call received.
- `ipc_wait` returns 0 with the notification's bits in RDI, waiting until there are some.
  `ipc_poll` does not wait, and leaves 0 in RDI if there are none.
- `ipc_set_buffer` names the thread's IPC buffer, 64-byte aligned, or none with `NULL`. A thread
  without one can only send and receive what fits in registers.

## Capabilities (family `0x1`)

```c
/* 0x100 */ cap_t cap_create(enum cap_type type, uint64_t size, enum page_size page_size);
/* 0x101 */ cap_t cap_mint(cap_t src_cspace, cap_t src, cap_t dst_cspace,
                           uint8_t rights, uint64_t badge, uint8_t flags);
/* 0x102 */ cap_t cap_copy(cap_t src_cspace, cap_t src, cap_t dst_cspace, uint8_t rights_mask);
/* 0x103 */ cap_t cap_move(cap_t src_cspace, cap_t src, cap_t dst_cspace);
/* 0x104 */ long  cap_delete(cap_t cspace, cap_t cap);
/* 0x105 */ long  cap_revoke(cap_t cspace, cap_t original);
```

- `cap_create` returns the original capability to a new object, with every right, in the caller's
  CSpace. `size` and `page_size` are only for `CAP_VM_OBJECT`. The object counts against the
  caller's quota until it is gone: `-QuotaExceeded` when there is no room.
- `cap_mint` sets `badge` unless it is 0, which takes `RIGHT_MINT`.
- `cap_revoke` returns how many capabilities it revoked.
- Taking from a CSpace other than the caller's takes `RIGHT_READ` on it (`RIGHT_WRITE` for
  `cap_move`), putting into one or deleting from one `RIGHT_WRITE`, revoking in one
  `RIGHT_REVOKE`.

## Memory (family `0x2`)

```c
/* 0x200 */ void*    mem_allocate(cap_t process, size_t size, uint32_t access,
                                  enum page_size page_size, void* hint);
/* 0x201 */ long     mem_protect(cap_t process, void* address, size_t size, uint32_t access);
/* 0x202 */ long     mem_unmap(cap_t process, void* address);
/* 0x203 */ void*    mem_map_object(cap_t process, cap_t object, uint32_t access,
                                    size_t first_page, size_t pages, void* hint);
/* 0x204 */ cap_t    mem_derive_object(cap_t object, size_t first_page, size_t pages,
                                       uint8_t rights);
/* 0x205 */ long     mem_resize_object(cap_t object, size_t pages);
/* 0x206 */ uint64_t mem_translate_object(cap_t object, size_t page);
/* 0x207 */ long     mem_info(struct memory_info* out);
```

- `mem_allocate` takes `ACCESS_READ`, `ACCESS_WRITE`, `ACCESS_EXECUTE`, `ACCESS_POPULATE` and
  `ACCESS_STACK`. All of the range counts against the quota from the start.
- `mem_protect` takes `ACCESS_READ`, `ACCESS_WRITE` and `ACCESS_EXECUTE`. Only anonymous memory can
  gain a permission; anything else can only lose them.
- `mem_unmap` unmaps whatever starts at `address`.
- `mem_map_object` takes `ACCESS_READ`, `ACCESS_WRITE`, `ACCESS_EXECUTE` and `ACCESS_POPULATE`,
  which the object's capability has to allow. `pages` of 0 maps to the end.
- A `hint` of `NULL` lets the kernel pick the address; otherwise it is where the mapping goes.
- `mem_translate_object` returns the physical address of the page, filling it in first.

## Devices (family `0x3`)

```c
/* 0x300 */ long  dev_irq_bind(cap_t irq, cap_t notification, uint64_t badge);
/* 0x301 */ long  dev_irq_ack(cap_t irq);
/* 0x302 */ void* dev_mmio_map(cap_t process, cap_t region, size_t offset, size_t length,
                               void* hint);
/* 0x303 */ cap_t dev_mmio_derive(cap_t region, size_t offset, size_t size,
                                  enum cache_mode cache, uint8_t rights);
/* 0x304 */ void* dev_dma_alloc(cap_t pool, size_t size, enum cache_mode cache,
                                struct dma_segment* segments, size_t max_segments);
/* 0x305 */ long  dev_dma_free(cap_t pool, void* buffer);
```

- `dev_irq_bind` with a `notification` of 0 unbinds. A `badge` of 0 uses the notification
  capability's badge, or bit 0 if it has none.
- `dev_mmio_map` with a `length` of 0 maps to the end of the region.
- `dev_dma_alloc` writes where each page of the buffer is to `segments`, as far as `max_segments`
  goes, 32 at most, and returns the physical address of the first page in RDI.

## Processes (family `0x4`)

```c
/* 0x400 */ cap_t proc_create(const char* name, size_t name_len, cap_t parent);
/* 0x401 */ cap_t proc_fork(const char* name, size_t name_len, void* entry, uint64_t arg);
/* 0x402 */ long  proc_exit(cap_t process, int status);
/* 0x403 */ pid_t proc_wait(cap_t process, pid_t child, uint64_t timeout_ns, int* status);
/* 0x404 */ pid_t proc_id(void);
/* 0x405 */ cap_t proc_grant(cap_t process, cap_t cap, uint8_t rights_mask, uint64_t badge);
/* 0x406 */ cap_t proc_fork_here(const char* name, size_t name_len);
```

- Names longer than 23 bytes are cut short.
- `proc_fork` starts the child's one thread at `entry`, with `arg` in RDI, and an empty CSpace.
- `proc_wait` waits for `child` (~0 for any) for `timeout_ns`, or for as long as it takes with 0,
  writes its exit status to `status` unless that is `NULL`, and returns its ID.
- `proc_grant` copies `cap` from the caller's CSpace into the process's, minted with `badge`
  unless that is 0, and returns its ID there.
- `proc_fork_here` returns twice. In the caller it returns a capability to the child, with the
  child's ID in RDI. In the child, whose one thread carries on from here, it returns 0, with 0 in
  RDI and every other register as it was. The child has copies of the caller's memory, of its
  CSpace under the same IDs, and of the calling thread's IPC buffer, FPU state, FS and GS bases
  and PKRU.
- If the caller cannot be handed the child's capability, the child is ended, and the call fails.

## Threads (family `0x5`)

```c
/* 0x500 */ cap_t thread_spawn(cap_t process, const char* name, size_t name_len, void* entry,
                               uint64_t arg);
/* 0x501 */ long  thread_exit(cap_t thread);
/* 0x502 */ long  thread_configure(cap_t thread_or_process, cap_t notification,
                                   cap_t fault_endpoint, uint64_t fault_badge);
/* 0x503 */ long  thread_yield(void);
/* 0x504 */ long  thread_set_priority(void);
/* 0x505 */ tid_t thread_id(void);
/* 0x506 */ long  thread_stats(cap_t thread, struct thread_stats* out);
```

- `thread_spawn` starts the thread at `entry`, with `arg` in RDI. As with processes, names longer
  than 23 bytes are cut short.
- `thread_configure` binds `notification` to the thread unless it is 0, and sends its faults to
  `fault_endpoint` with `fault_badge`, or nowhere with 0. Given a process, it sets where faults of
  its threads go, and `notification` must be 0.
- `thread_set_priority` is reserved, and returns `-NotImplemented`.

## Time (family `0x6`)

```c
/* 0x600 */ uint64_t time_now(void);
/* 0x601 */ long     time_sleep(uint64_t ns, uint64_t slack_ns, uint64_t flags);
```

- `time_now` returns nanoseconds since boot.
- `time_sleep` sleeps for `ns`, or with bit 0 of `flags` set until `ns` nanoseconds since boot.
  `slack_ns` more is fine too, if that saves an interrupt. It returns `-WouldBlock` if the sleep
  was cut short.

## Architecture (family `0x7`)

```c
/* 0x700 */ long  arch_set_fs_base(void* base);
/* 0x701 */ void* arch_get_fs_base(void);
/* 0x702 */ long  arch_set_gs_base(void* base);
/* 0x703 */ void* arch_get_gs_base(void);
/* 0x704 */ long  arch_enable_amx(void);
```

- A base is a user address, or `NULL`.
- `arch_enable_amx` returns `-NotImplemented` on a CPU without AMX.

## Debugging (family `0x8`)

```c
/* 0x800 */ long debug_log(const char* text, size_t len);
```

- Writes up to 512 bytes of `text` to the kernel log, as one line.

## Errors

A Failed call returns, negated.

| Error | Code | Usually means |
|---|---|---|
| `NotImplemented` | `0x1001` | No such call, or nothing behind it yet |
| `InvalidState` | `0x1002` | Not now: no IPC context, a process that is exiting, a cleared CSpace |
| `InvalidFlags` | `0x1003` | A flag or right the call does not know |
| `InvalidArgument` | `0x1004` | A bad value, a capability of the wrong type, or an address outside user space |
| `Timeout` | `0x1005` | Waited as long as asked |
| `NotFound` | `0x1006` | An empty, stale or revoked capability, or no such child |
| `OutOfMemory` | `0x2001` | The kernel has no memory for it |
| `OutOfBounds` | `0x2002` | Past the end of an object or region |
| `InvalidAlignment` | `0x2003` | An address or size that is not aligned the way it has to be |
| `AlreadyMapped` | `0x2004` | Something is mapped at the hint already |
| `NotMapped` | `0x2005` | Nothing is mapped there |
| `StackOverflow` | `0x2006` | A stack that cannot grow any further |
| `MemoryProtectionFault` | `0x2008` | User memory the kernel could not read or write |
| `QuotaExceeded` | `0x2009` | More than the caller's quota has room for |
| `WouldBlock` | `0x3003` | A call that does not wait would have had to |
| `SecurityViolation` | `0x4001` | A badge that may not be changed |
| `AccessDenied` | `0x4002` | A capability without the rights the call takes |
| `PrivilegeEscalationDetected` | `0x4004` | Asked for rights the source does not have |
