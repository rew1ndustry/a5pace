# Portability

The core (store, tank, engine, tick loop) is plain C99 plus stdio, with sqlite and libFLAC, both of
which build on Windows, macOS, Linux and AIX. Platform differences live in the shells and edges.

Rules the core follows:
- **Byte order.** Everything on disk and in the pipe is little endian, read and written through
  `plat.h` (`asp_ld32/64`, `asp_st32/64`). AIX/POWER is big endian, so no raw struct or array
  dumps. FLAC itself is byte-order neutral.
- **Large files.** `ASP_FSEEK/ASP_FTELL` map to `fseeko/ftello` or `_fseeki64/_ftelli64`.
- **No** mmap, flock, fork, signals or sockets in the core. One aspace per user is a single
  writer; the shell prevents a second one (a lock file in the shell, not the core).
- **Binary pipes.** On Windows the shell puts stdin/stdout in binary mode before starting the pipe.
- **Directories and fsync.** The store uses `mkdir`, `rename`, `fsync` (POSIX). Windows needs a
  five-line shim (`_mkdir`, `MoveFileEx`, `_commit`); it will live in `plat.h` when the first
  Windows build is attempted.
- **Arithmetic.** No shifts of negative integers; the loop gain is a double multiply on the way out.

Not yet built or tested on Windows, macOS or AIX; Linux only so far.
