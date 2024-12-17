//===- ProgramExtra.h - Program extra utilities -----------------*- C++ -*-===//
//
// Copyright 2024 Ehud Katz <ehudkatz@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#ifndef PROGRAMEXTRA_H
#define PROGRAMEXTRA_H

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

namespace llvm {

/// This function executes the program using the arguments provided.
/// The behaviour is exactly the same as with `llvm::sys::ExecuteAndWait`, but
/// with stdin given by a string, while stdout and stderr are copied to a given
/// string.
Expected<std::unique_ptr<llvm::MemoryBuffer>> executeAndWaitWithPipe(
    StringRef program, ArrayRef<StringRef> args,
    std::optional<ArrayRef<StringRef>> env = std::nullopt,
    std::optional<StringRef> input = std::nullopt,
    std::unique_ptr<llvm::MemoryBuffer> *errMBuf = nullptr,
    unsigned secondsToWait = 0, unsigned memoryLimit = 0,
    bool *executionFailed = nullptr,
    std::optional<llvm::sys::ProcessStatistics> *procStat = nullptr,
    llvm::BitVector *affinityMask = nullptr);

} // namespace llvm

#endif // PROGRAMEXTRA_H
