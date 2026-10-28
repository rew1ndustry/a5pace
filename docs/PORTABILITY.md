# Portability

The core (store, tank, engine, tick loop) is plain C99 plus stdio, with sqlite and libFLAC, both of
which build on Windows, macOS, Linux and AIX. Platform differences live in the shells and edges.

Rules the core follows:
- **Byte order.** Everything on disk and in the pipe is little endian, read and written through
  `plat.h` (`asp_ld32/64`, `asp_st32/64`). AIX/POWER is big endian, so no raw struct or array
  dumps. FLAC itself is byte-order neutral.
- **Large files.** `ASP_FSEEK/ASP_FTELL` map to `fseeko/ftello` or `_fseeki64/_ftelli64`.
- **No** mmap, flock, fork or sockets in the core. One aspace per user is a single
  writer; the shell prevents a second one (a lock file in the shell, not the core).
- **One servant thread.** The keeper (`keeper.c`) is the only thread: pthreads, a mutex, a condition variable, and nothing
  shared with the core but its queue. It does the things that wait on the disk (see docs/STORE-DESIGN.md).
- **Binary pipes.** On Windows the shell puts stdin/stdout in binary mode before starting the pipe.
- **Files and syncing.** The take files and the store use POSIX calls: `open`, `pwrite`, `fdatasync`, `fsync` (also of a directory),
  `rename`, `mkdir`, `fstat`, `kill(pid, 0)` (to see whether a writer is alive: crash detection), `nanosleep`; and, only where
  they exist, `sync_file_range` (Linux) and `posix_fadvise`. Windows needs a shim for these and for the keeper's pthreads
  (`_mkdir`, `MoveFileEx`, `_commit`, `CreateThread`, `CRITICAL_SECTION`); it will live in `plat.h` when the first
  Windows build is attempted. macOS has no `fdatasync` (use `fcntl(F_FULLFSYNC)`) and no `sync_file_range`; AIX is untried.
- **The codec** is chosen at build time (`make CODEC=flac` or `raw`, include/codec.h). Nothing outside `src/codec_*.c` knows what the bytes are.
- **The gate** is built with NEON flags only on armv7 (the Makefile checks `uname -m`); elsewhere it is plain `-O2`.
- **Arithmetic.** Level shifts use `>>` on negative integers, arithmetic on every compiler we
  target; `test_tank` checks it.

Not yet built or tested on Windows, macOS or AIX; Linux only so far (x86-64, and armv7 on a cubieboard). Help with other platforms is welcome: the shim
list above is the work.
