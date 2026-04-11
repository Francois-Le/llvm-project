//===--- BuildIndex.cpp - Build background index from command line ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provides the --build-index mode for clangd: instead of running as a language
// server, discover all translation units from the compilation database, build
// or update the background index cache (.cache/clangd/index/), print progress
// to stdout, and exit.
//
// This reuses the same BackgroundIndex infrastructure that runs during normal
// clangd operation, producing an identical on-disk shard cache.
//
//===----------------------------------------------------------------------===//

#include "ClangdLSPServer.h"
#include "CompileCommands.h"
#include "Config.h"
#include "GlobalCompilationDatabase.h"
#include "index/Background.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "support/Shutdown.h"
#include "support/ThreadsafeFS.h"
#include "support/Trace.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include <atomic>
#include <chrono>
#include <string>

namespace clang {
namespace clangd {

bool buildIndex(const ThreadsafeFS &TFS,
                const ClangdLSPServer::Options &Opts) {
  llvm::SmallString<256> CWD;
  if (auto Err = llvm::sys::fs::current_path(CWD)) {
    elog("Failed to get current directory: {0}", Err.message());
    return false;
  }

  llvm::outs() << "Building background index...\n";
  log("Building background index in {0}", CWD);

  // Set up the compilation database, reusing the same configuration as
  // normal clangd operation (--compile-commands-dir, .clangd config, etc.).
  DirectoryBasedGlobalCompilationDatabase::Options CDBOpts(TFS);
  if (Opts.ConfigProvider)
    CDBOpts.ContextProvider =
        ClangdServer::createConfiguredContextProvider(Opts.ConfigProvider,
                                                     nullptr);

  // In build-index mode, lock CDB discovery to a single directory to prevent
  // loading multiple compilation databases when indexed files have paths in
  // subdirectories that contain their own compile_commands.json.
  // If --compile-commands-dir was specified (via config), it takes priority.
  // Otherwise, default to CWD.
  if (!CDBOpts.CompileCommandsDir)
    CDBOpts.CompileCommandsDir = CWD.str().str();

  DirectoryBasedGlobalCompilationDatabase BaseCDB(CDBOpts);

  // Wrap in OverlayCDB with CommandMangler to apply the same compile command
  // adjustments as normal clangd operation (resource dir, input stripping,
  // driver resolution, etc.). Without this, flags like -msse4.2 would be
  // mishandled by clang-cl, and builtin headers wouldn't be found.
  auto Mangler = CommandMangler::detect();
  if (Opts.ResourceDir)
    Mangler.ResourceDir = *Opts.ResourceDir;
  OverlayCDB CDB(&BaseCDB, /*FallbackFlags=*/{}, std::move(Mangler));

  bool IsTerminal = llvm::outs().is_displayed();
  std::atomic<unsigned> LastPrintedCompleted{0};
  auto StartTime = std::chrono::steady_clock::now();

  // Set up BackgroundIndex with progress reporting to stdout.
  BackgroundIndex::Options BGOpts;
  BGOpts.ThreadPoolSize = std::max(Opts.AsyncThreadsCount, 1u);
  // Use normal priority since this is the primary (foreground) task.
  BGOpts.IndexingPriority = llvm::ThreadPriority::Default;
  BGOpts.SupportContainedRefs = Opts.EnableOutgoingCalls;
  if (CDBOpts.ContextProvider)
    BGOpts.ContextProvider = CDBOpts.ContextProvider;

  BGOpts.OnProgress = [&](BackgroundQueue::Stats S) {
    if (S.Enqueued == 0)
      return;
    unsigned Pct = 100 * S.Completed / S.Enqueued;
    if (IsTerminal) {
      llvm::outs() << "\r[" << S.Completed << "/" << S.Enqueued << "] ("
                   << S.Active << " active) " << Pct << "%  ";
      llvm::outs().flush();
    } else {
      // For non-terminal output, print periodic line-based updates.
      unsigned Prev = LastPrintedCompleted.load();
      unsigned Step = std::max(1u, S.Enqueued / 20); // ~5% increments
      if (S.Completed >= Prev + Step || S.Completed == S.Enqueued) {
        LastPrintedCompleted.store(S.Completed);
        llvm::outs() << "[" << S.Completed << "/" << S.Enqueued << "] " << Pct
                     << "%\n";
      }
    }
  };

  BackgroundIndex Idx(
      TFS, CDB,
      Opts.KeepShardHistory
          ? BackgroundIndexStorage::createDiskBackedHistoryStorageFactory(
                [&CDB](llvm::StringRef File) {
                  return CDB.getProjectInfo(File);
                })
          : BackgroundIndexStorage::createDiskBackedStorageFactory(
                [&CDB](llvm::StringRef File) {
                  return CDB.getProjectInfo(File);
                }),
      std::move(BGOpts));

  // Trigger CDB discovery by querying a compile command for a synthetic path
  // under CWD. This causes DirectoryBasedGlobalCompilationDatabase to find
  // and broadcast all files from the compilation database.
  llvm::SmallString<256> TriggerPath(CWD);
  llvm::sys::path::append(TriggerPath, "__clangd_build_index_trigger__.cpp");
  CDB.getCompileCommand(TriggerPath);

  // Wait for the CDB broadcast to complete - it happens asynchronously.
  CDB.blockUntilIdle(Deadline::infinity());

  // Wait for background indexing to complete, checking for shutdown requests.
  // blockUntilIdleForTest returns true immediately if the queue is empty (no
  // files discovered), or waits until all tasks complete.
  while (!Idx.blockUntilIdleForTest(/*TimeoutSeconds=*/1)) {
    if (shutdownRequested())
      break;
  }

  if (IsTerminal)
    llvm::outs() << "\n";

  auto EndTime = std::chrono::steady_clock::now();
  auto Duration = std::chrono::duration_cast<std::chrono::seconds>(
      EndTime - StartTime);

  if (shutdownRequested()) {
    llvm::outs() << "Indexing interrupted.\n";
    return false;
  }

  llvm::outs() << "Indexing complete in " << Duration.count() << "s.\n";
  return true;
}

} // namespace clangd
} // namespace clang
