//===- ProgramExtra.cpp - Program extra utilities definitions -------------===//
//
// Copyright 2024 Ehud Katz <ehudkatz@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "ProgramExtra.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Config/config.h"
#include <unistd.h>

using namespace llvm;
using namespace llvm::sys;

namespace {

struct Pipe : public std::array<int, 2> {
  enum { ReadId = 0, WriteId = 1 };

  Pipe() { fill(-1); }

  operator bool() const { return front() != -1; }

  int getRead() const { return at(ReadId); }

  int getWrite() const { return at(WriteId); }

  void closeRead() {
    if (at(ReadId) != -1)
      fs::closeFile(at(ReadId));
  }

  void closeWrite() {
    if (at(WriteId) != -1)
      fs::closeFile(at(WriteId));
  }

  void close() {
    closeRead();
    closeWrite();
  }

  Error open() {
    if (::pipe(data()) == -1) {
      std::error_code ec(errno, std::generic_category());
      return make_error<StringError>(
          "Could not create in pipe: " + ec.message(), ec);
    }
    return Error::success();
  }
};

using PipeArray = std::array<Pipe, 3>;

} // anonymous namespace

// We need to override the `Execute`, `RedirectIO` and `RedirectIO_PS` to extend
// their functionality to support redirecting the pipe FDs.

bool Execute(ProcessInfo &pi, StringRef program, ArrayRef<StringRef> args,
             std::optional<ArrayRef<StringRef>> env,
             ArrayRef<std::optional<StringRef>> redirects, unsigned memoryLimit,
             std::string *errMsg, BitVector *affinityMask);

#define ExecuteProcessInfo ExecuteImpl(PipeArray &pipes, ProcessInfo
#define ExecutePI Execute(PI

#define Execute(PI, Program, Args, Env, Redirects, MemoryLimit, ErrMsg,        \
                AffinityMask)                                                  \
  Execute##PI, Program, Args, Env,Redirects, MemoryLimit, ErrMsg, AffinityMask)

static bool RedirectIO(PipeArray &pipes, std::optional<StringRef> path, int fd,
                       std::string *errMsg);

#define RedirectIOstd RedirectIO_NoPipe(std
#define RedirectIORedirects RedirectIO(pipes, Redirects

#define RedirectIO(Path, FD, ErrMsg) RedirectIO##Path, FD, ErrMsg)

#ifdef HAVE_POSIX_SPAWN
#include <spawn.h>

static bool RedirectIO_PS(PipeArray &pipes, const std::string *path, int fd,
                          std::string *errMsg,
                          posix_spawn_file_actions_t *fileActions);

#define RedirectIO_PSconst RedirectIO_PS_NoPipe(const
#define RedirectIO_PSRedirectsStr RedirectIO_PS(pipes, RedirectsStr

#define RedirectIO_PS(Path, FD, ErrMsg, FileActions)                           \
  RedirectIO_PS##Path, FD, ErrMsg, FileActions)
#endif

#include "lib/Support/Program.cpp"

#undef Execute
#undef RedirectIO
#ifdef HAVE_POSIX_SPAWN
#undef RedirectIO_PS
#endif

bool Execute(ProcessInfo &pi, StringRef program, ArrayRef<StringRef> args,
             std::optional<ArrayRef<StringRef>> env,
             ArrayRef<std::optional<StringRef>> redirects, unsigned memoryLimit,
             std::string *errMsg, BitVector *affinityMask) {
  PipeArray pipes;
  return ExecuteImpl(pipes, pi, program, args, env, redirects, memoryLimit,
                     errMsg, affinityMask);
}

static bool RedirectIO(PipeArray &pipes, std::optional<StringRef> path, int fd,
                       std::string *errMsg) {
  auto &pipe = pipes[fd];
  if (!pipe)
    return RedirectIO_NoPipe(path, fd, errMsg);

  unsigned dupFd, unusedFd;
  if (fd == STDIN_FILENO) {
    dupFd = Pipe::ReadId;
    unusedFd = Pipe::WriteId;
  } else {
    dupFd = Pipe::WriteId;
    unusedFd = Pipe::ReadId;
  }

  // Close the unused endpoint.
  close(pipe[unusedFd]);

  // Bind `fd` to the requested end of the pipe.
  bool failed = dup2(pipe[dupFd], fd) == -1;
  if (failed)
    MakeErrMsg(errMsg, "Cannot dup2");

  // Close the original descriptor.
  close(pipe[dupFd]);
  return failed;
}

#ifdef HAVE_POSIX_SPAWN

static bool RedirectIO_PS(PipeArray &pipes, const std::string *path, int fd,
                          std::string *errMsg,
                          posix_spawn_file_actions_t *fileActions) {
  auto &pipe = pipes[fd];
  if (!pipe)
    return RedirectIO_PS_NoPipe(path, fd, errMsg, fileActions);

  unsigned dupFd, unusedFd;
  if (fd == STDIN_FILENO) {
    dupFd = Pipe::ReadId;
    unusedFd = Pipe::WriteId;
  } else {
    dupFd = Pipe::WriteId;
    unusedFd = Pipe::ReadId;
  }

  // Close the unused endpoint.
  posix_spawn_file_actions_addclose(fileActions, pipe[unusedFd]);

  // Bind `fd` to the requested end of the pipe.
  int err = posix_spawn_file_actions_adddup2(fileActions, pipe[dupFd], fd);
  if (err)
    MakeErrMsg(errMsg, "Cannot posix_spawn_file_actions_adddup2", err);

  // Close the original descriptor.
  posix_spawn_file_actions_addclose(fileActions, pipe[dupFd]);
  return err != 0;
}

#endif

Expected<std::unique_ptr<MemoryBuffer>> llvm::executeAndWaitWithPipe(
    StringRef program, ArrayRef<StringRef> args,
    std::optional<ArrayRef<StringRef>> env, std::optional<StringRef> input,
    std::unique_ptr<MemoryBuffer> *errBuf, unsigned secondsToWait,
    unsigned memoryLimit, bool *executionFailed,
    std::optional<ProcessStatistics> *procStat, BitVector *affinityMask) {
  PipeArray pipes;
  auto autoClose = make_scope_exit([&pipes] {
    for (Pipe &pipe : pipes)
      pipe.close();
  });

  // If `errBuf` is valid, then we expect to return a separate buffer for
  // stderr, otherwise, it is redirected to stdout, and we do not need to open a
  // pipe for the stderr (the 3rd pipe).
  size_t numPipes = errBuf ? 3 : 2;

  for (size_t i = 0; i < numPipes; ++i)
    if (Error err = pipes[i].open())
      return std::move(err);

  ProcessInfo pi;
  std::optional<StringRef> redirects[] = {
      StringRef("|0"),
      StringRef("|1"),
      StringRef(errBuf ? "|2" : "|1"),
  };
  std::string errMsg;
  bool failed = !ExecuteImpl(pipes, pi, program, args, env, redirects,
                             memoryLimit, &errMsg, affinityMask);
  if (executionFailed)
    *executionFailed = failed;
  if (failed)
    return make_error<StringError>(
        errMsg, std::make_error_code(std::errc::invalid_argument));

  pipes[STDIN_FILENO].closeRead();
  pipes[STDOUT_FILENO].closeWrite();
  pipes[STDERR_FILENO].closeWrite();

  if (input)
    raw_fd_ostream(pipes[STDIN_FILENO].getWrite(), false) << *input;
  pipes[STDIN_FILENO].closeWrite();

  // Two output buffers: first for stdout, second for stderr.
  std::unique_ptr<MemoryBuffer> outputBufs[2];
  for (size_t i = 1; i < numPipes; ++i) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> memBufOrErr =
        MemoryBuffer::getOpenFile(pipes[i].getRead(), {}, -1);
    if (std::error_code ec = memBufOrErr.getError())
      return make_error<StringError>("Could not open pipe: " + ec.message(),
                                     ec);

    outputBufs[i - 1] = std::move(*memBufOrErr);
  }

  if (errBuf)
    *errBuf = std::move(outputBufs[1]);

  pi =
      Wait(pi, secondsToWait == 0 ? std::nullopt : std::optional(secondsToWait),
           &errMsg, procStat);

  if (pi.ReturnCode < 0)
    return make_error<StringError>(
        errMsg, std::make_error_code(std::errc::invalid_argument));

  return std::move(outputBufs[0]);
}
