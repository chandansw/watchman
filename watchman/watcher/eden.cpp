/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cpptoml.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/async/PooledRequestChannel.h>
#include <thrift/lib/cpp2/async/ReconnectingRequestChannel.h>
#include <thrift/lib/cpp2/async/RetryingRequestChannel.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>
#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>
#include "eden/fs/service/gen-cpp2/StreamingEdenService.h"
#include "watchman/ChildProcess.h"
#include "watchman/Errors.h"
#include "watchman/LRUCache.h"
#include "watchman/QueryableView.h"
#include "watchman/ThreadPool.h"
#include "watchman/fs/FSDetect.h"
#include "watchman/fs/FileDescriptor.h"
#include "watchman/query/GlobTree.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryContext.h"
#include "watchman/query/eval.h"
#include "watchman/root/Root.h"
#include "watchman/scm/SCM.h"
#include "watchman/thirdparty/wildmatch/wildmatch.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watcher/WatcherRegistry.h"

using apache::thrift::TApplicationException;
using namespace facebook::eden;
using folly::AsyncSocket;
using folly::to;
using std::make_unique;

namespace {
using EdenDtype = facebook::eden::Dtype;
using watchman::DType;

DType getDTypeFromEden(EdenDtype dtype) {
  // TODO: Eden guarantees that dtypes have consistent values on all platforms,
  // including Windows. If we made Watchman guarantee that too, this could be
  // replaced with a static_cast.

  switch (dtype) {
    case EdenDtype::UNKNOWN:
      return DType::Unknown;
    case EdenDtype::FIFO:
      return DType::Fifo;
    case EdenDtype::CHAR:
      return DType::Char;
    case EdenDtype::DIR:
      return DType::Dir;
    case EdenDtype::BLOCK:
      return DType::Block;
    case EdenDtype::REGULAR:
      return DType::Regular;
    case EdenDtype::LINK:
      return DType::Symlink;
    case EdenDtype::SOCKET:
      return DType::Socket;
    case EdenDtype::WHITEOUT:
      return DType::Whiteout;
  }
  return DType::Unknown;
}

SyncBehavior getSyncBehavior() {
  // Use a no-sync behavior as syncToNow will be called if a synchronization is
  // necessary which will do the proper synchronization.
  auto sync = SyncBehavior{};
  sync.syncTimeoutSeconds() = 0;
  return sync;
}

} // namespace

namespace watchman {
namespace {
struct NameAndDType {
  std::string name;
  DType dtype;

  explicit NameAndDType(const std::string& name, DType dtype = DType::Unknown)
      : name(name), dtype(dtype) {}
};

/** This is a helper for settling out subscription events.
 * We have a single instance of the callback object that we schedule
 * each time we get an update from the eden server.  If we are already
 * scheduled we will cancel it and reschedule it.
 */
class SettleCallback : public folly::HHWheelTimer::Callback {
 public:
  SettleCallback(folly::EventBase* eventBase, std::shared_ptr<Root> root)
      : eventBase_(eventBase), root_(std::move(root)) {}

  void timeoutExpired() noexcept override {
    try {
      auto settledPayload = json_object({{"settled", json_true()}});
      root_->unilateralResponses->enqueue(std::move(settledPayload));
    } catch (const std::exception& exc) {
      log(ERR,
          "error while dispatching settle payload; cancel watch: ",
          exc.what(),
          "\n");
      eventBase_->terminateLoopSoon();
    }
  }

  void callbackCanceled() noexcept override {
    // We must override this because the default is to call timeoutExpired().
    // We don't want that to happen because we're only canceled in the case
    // where we want to delay the timeoutExpired() callback.
  }

 private:
  folly::EventBase* eventBase_;
  std::shared_ptr<Root> root_;
};

// Resolve the eden socket; On POSIX systems we use the .eden dir that is
// present in every dir of an eden mount to locate the symlink to the socket.
// On Windows systems, .eden is only present in the repo root and contains
// the toml config file with the path to the socket.
std::string resolveSocketPath(w_string_piece rootPath) {
#ifdef _WIN32
  auto configPath = to<std::string>(rootPath.view(), "/.eden/config");
  auto config = cpptoml::parse_file(configPath);

  return *config->get_qualified_as<std::string>("Config.socket");
#else
  auto path = to<std::string>(rootPath.view(), "/.eden/socket");
  // It is important to resolve the link because the path in the eden mount
  // may exceed the maximum permitted unix domain socket path length.
  // This is actually how things our in our integration test environment.
  return readSymbolicLink(path.c_str()).string();
#endif
}

folly::SocketAddress getEdenSocketAddress(w_string_piece rootPath) {
  folly::SocketAddress addr;

  auto socketPath = resolveSocketPath(rootPath);
  addr.setFromPath(to<std::string>(socketPath));
  return addr;
}

/** Create a thrift client that will connect to the eden server associated
 * with the current user. */
std::unique_ptr<StreamingEdenServiceAsyncClient> getEdenClient(
    std::shared_ptr<apache::thrift::RequestChannel> channel) {
  return make_unique<StreamingEdenServiceAsyncClient>(std::move(channel));
}

class GetJournalPositionCallback : public folly::HHWheelTimer::Callback {
 public:
  GetJournalPositionCallback(
      folly::EventBase* eventBase,
      std::shared_ptr<apache::thrift::RequestChannel> thriftChannel,
      std::string mountPoint)
      : eventBase_{eventBase},
        thriftChannel_{std::move(thriftChannel)},
        mountPoint_{std::move(mountPoint)} {}

  void timeoutExpired() noexcept override {
    try {
      auto edenClient = getEdenClient(thriftChannel_);

      // Calling getCurrentJournalPosition will allow EdenFS to send new
      // notification about files changed.
      JournalPosition journal;
      edenClient->sync_getCurrentJournalPosition(journal, mountPoint_);
    } catch (const std::exception& exc) {
      log(ERR,
          "error while getting EdenFS's journal position; cancel watch: ",
          exc.what(),
          "\n");
      eventBase_->terminateLoopSoon();
    }
  }

 private:
  folly::EventBase* eventBase_;
  std::shared_ptr<apache::thrift::RequestChannel> thriftChannel_;
  std::string mountPoint_;
};

class EdenFileResult : public FileResult {
 public:
  EdenFileResult(
      const w_string& rootPath,
      std::shared_ptr<apache::thrift::RequestChannel> thriftChannel,
      const w_string& fullName,
      JournalPosition* position = nullptr,
      bool isNew = false,
      DType dtype = DType::Unknown)
      : rootPath_(rootPath),
        thriftChannel_{std::move(thriftChannel)},
        fullName_(fullName),
        dtype_(dtype) {
    otime_.ticks = ctime_.ticks = 0;
    otime_.timestamp = ctime_.timestamp = 0;
    if (position) {
      otime_.ticks = *position->sequenceNumber();
      if (isNew) {
        // the "ctime" in the context of FileResult represents the point
        // in time that we saw the file transition !exists -> exists.
        // We don't strictly know the point at which that happened for results
        // returned from eden, but it will tell us whether that happened in
        // a given since query window by listing the file in the created files
        // set.  We set the isNew flag in this case.  The goal here is to
        // ensure that the code in query/eval.cpp considers us to be new too,
        // and that works because we set the created time ticks == the last
        // change tick.  The logic in query/eval.cpp will consider this to
        // be new because the ctime > lower bound in the since query.
        // When isNew is not set our ctime tick value is initialized to
        // zero which always fails that is_new check.
        ctime_.ticks = otime_.ticks;
      }
    }
  }

  std::optional<FileInformation> stat() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::FullFileInformation);
      return std::nullopt;
    }
    return stat_;
  }

  std::optional<DType> dtype() override {
    // We're using Unknown as the default value to avoid also wrapping
    // this value up in an Optional in our internal storage.
    // In theory this is ambiguous, but in practice Eden will never
    // return Unknown for dtype values so this is safe to use with
    // impunity.
    if (dtype_ != DType::Unknown) {
      return dtype_;
    }
    if (stat_.has_value()) {
      return stat_->dtype();
    }
    accessorNeedsProperties(FileResult::Property::FileDType);
    return std::nullopt;
  }

  std::optional<size_t> size() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::Size);
      return std::nullopt;
    }
    return stat_->size;
  }

  std::optional<struct timespec> accessedTime() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::StatTimeStamps);
      return std::nullopt;
    }
    return stat_->atime;
  }

  std::optional<struct timespec> modifiedTime() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::StatTimeStamps);
      return std::nullopt;
    }
    return stat_->mtime;
  }

  std::optional<struct timespec> changedTime() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::StatTimeStamps);
      return std::nullopt;
    }
    return stat_->ctime;
  }

  w_string_piece baseName() override {
    return fullName_.piece().baseName();
  }

  w_string_piece dirName() override {
    return fullName_.piece().dirName();
  }

  void setExists(bool exists) noexcept {
    exists_ = exists;
    if (!exists) {
      stat_ = FileInformation::makeDeletedFileInformation();
    }
  }

  std::optional<bool> exists() override {
    if (!exists_.has_value()) {
      accessorNeedsProperties(FileResult::Property::Exists);
      return std::nullopt;
    }
    return exists_;
  }

  std::optional<w_string> readLink() override {
    if (symlinkTarget_.has_value()) {
      return symlinkTarget_;
    }
    accessorNeedsProperties(FileResult::Property::SymlinkTarget);
    return std::nullopt;
  }

  std::optional<ClockStamp> ctime() override {
    return ctime_;
  }

  std::optional<ClockStamp> otime() override {
    return otime_;
  }

  std::optional<FileResult::ContentHash> getContentSha1() override {
    if (!sha1_.has_value()) {
      accessorNeedsProperties(FileResult::Property::ContentSha1);
      return std::nullopt;
    }
    switch (sha1_->getType()) {
      // Copy thrift SHA1Result aka (std::string) into
      // watchman FileResult::ContentHash aka (std::array<uint8_t, 20>)
      case SHA1Result::Type::sha1: {
        auto& hash = sha1_->get_sha1();
        FileResult::ContentHash result;
        std::copy(hash.begin(), hash.end(), result.begin());

        return result;
      }

      // Thrift error occured
      case SHA1Result::Type::error: {
        auto& err = sha1_->get_error();
        XCHECK(err.errorCode_ref());
        throw std::system_error(
            *err.errorCode_ref(), std::generic_category(), *err.message_ref());
      }

      // Something is wrong with type union
      default:
        throw std::runtime_error(
            "Unknown thrift data for EdenFileResult::getContentSha1");
    }
  }

  void batchFetchProperties(
      const std::vector<std::unique_ptr<FileResult>>& files) override {
    std::vector<EdenFileResult*> getFileInformationFiles;
    std::vector<std::string> getFileInformationNames;
    // If only dtype and exists are needed, Eden has a cheaper API for
    // retrieving them.
    bool onlyEntryInfoNeeded = true;

    std::vector<EdenFileResult*> getShaFiles;
    std::vector<std::string> getShaNames;

    std::vector<EdenFileResult*> getSymlinkFiles;

    for (auto& f : files) {
      auto& edenFile = dynamic_cast<EdenFileResult&>(*f);

      auto relName = edenFile.fullName_.piece();

      if (rootPath_ == edenFile.fullName_) {
        // The root tree inode has changed
        relName = "";
      } else {
        // Strip off the mount point prefix for the names we're going
        // to pass to eden.  The +1 is its trailing slash.
        relName.advance(rootPath_.size() + 1);
      }

      if (edenFile.neededProperties() & FileResult::Property::SymlinkTarget) {
        // We need to know if the node is a symlink
        edenFile.accessorNeedsProperties(FileResult::Property::FileDType);

        getSymlinkFiles.emplace_back(&edenFile);
      }

      if (edenFile.neededProperties() &
          (FileResult::Property::FileDType | FileResult::Property::CTime |
           FileResult::Property::OTime | FileResult::Property::Exists |
           FileResult::Property::Size | FileResult::Property::StatTimeStamps |
           FileResult::Property::FullFileInformation)) {
        getFileInformationFiles.emplace_back(&edenFile);
        getFileInformationNames.emplace_back(relName.data(), relName.size());

        if (edenFile.neededProperties() &
            ~(FileResult::Property::FileDType | FileResult::Property::Exists)) {
          // We could maintain two lists and call both getFileInformation and
          // getEntryInformation in parallel, but in practice the set of
          // properties should usually be the same across all files.
          onlyEntryInfoNeeded = false;
        }
      }

      if (edenFile.neededProperties() & FileResult::Property::ContentSha1) {
        getShaFiles.emplace_back(&edenFile);
        getShaNames.emplace_back(relName.data(), relName.size());
      }

      // If we were to throw later in this method, we will have forgotten
      // the input set of properties, but it is ok: if we do decide to
      // re-evaluate after throwing, the accessors will set the mask up
      // accordingly and we'll end up calling back in here if needed.
      edenFile.clearNeededProperties();
    }

    auto client = getEdenClient(thriftChannel_);
    loadFileInformation(
        client.get(),
        rootPath_,
        getFileInformationNames,
        getFileInformationFiles,
        onlyEntryInfoNeeded);

    // TODO: add eden bulk readlink call
    loadSymlinkTargets(client.get(), getSymlinkFiles);

    if (!getShaFiles.empty()) {
      std::vector<SHA1Result> sha1s;
      client->sync_getSHA1(
          sha1s, std::string{rootPath_.view()}, getShaNames, getSyncBehavior());

      if (sha1s.size() != getShaFiles.size()) {
        log(ERR,
            "Requested SHA-1 of ",
            getShaFiles.size(),
            " but Eden returned ",
            sha1s.size(),
            " results -- ignoring");
      } else {
        auto sha1Iter = sha1s.begin();
        for (auto& edenFile : getShaFiles) {
          edenFile->sha1_ = *sha1Iter++;
        }
      }
    }
  }

 private:
  w_string rootPath_;
  std::shared_ptr<apache::thrift::RequestChannel> thriftChannel_;
  w_string fullName_;
  std::optional<FileInformation> stat_;
  std::optional<bool> exists_;
  ClockStamp ctime_;
  ClockStamp otime_;
  std::optional<SHA1Result> sha1_;
  std::optional<w_string> symlinkTarget_;
  DType dtype_{DType::Unknown};

  // Read the symlink targets for each of the provided `files`.  The files
  // had SymlinkTarget set in neededProperties prior to clearing it in
  // the batchFetchProperties() method that calls us, so we know that
  // we unconditionally need to read these links.
  static void loadSymlinkTargets(
      StreamingEdenServiceAsyncClient*,
      const std::vector<EdenFileResult*>& files) {
    for (auto& edenFile : files) {
      if (!edenFile->stat_->isSymlink()) {
        // If this file is not a symlink then we immediately yield
        // a nullptr w_string instance rather than propagating an error.
        // This behavior is relied upon by the field rendering code and
        // checked in test_symlink.py.
        edenFile->symlinkTarget_ = w_string();
        continue;
      }
      edenFile->symlinkTarget_ = readSymbolicLink(edenFile->fullName_.c_str());
    }
  }

  static void loadFileInformation(
      StreamingEdenServiceAsyncClient* client,
      const w_string& rootPath,
      const std::vector<std::string>& names,
      const std::vector<EdenFileResult*>& outFiles,
      bool onlyEntryInfoNeeded) {
    w_assert(
        names.size() == outFiles.size(), "names.size must == outFiles.size");
    if (names.empty()) {
      return;
    }

    auto applyResults = [&](const auto& edenInfo) {
      if (names.size() != edenInfo.size()) {
        log(ERR,
            "Requested file information of ",
            names.size(),
            " files but Eden returned information for ",
            edenInfo.size(),
            " files. Treating missing entries as missing files.");
      }

      auto infoIter = edenInfo.begin();
      for (auto& edenFileResult : outFiles) {
        if (infoIter == edenInfo.end()) {
          edenFileResult->setExists(false);
        } else {
          edenFileResult->applyInformationOrError(*infoIter);
          ++infoIter;
        }
      }
    };

    if (onlyEntryInfoNeeded) {
      std::vector<EntryInformationOrError> info;
      try {
        client->sync_getEntryInformation(
            info, std::string{rootPath.view()}, names, getSyncBehavior());
        applyResults(info);
        return;
      } catch (const TApplicationException& ex) {
        if (TApplicationException::UNKNOWN_METHOD != ex.getType()) {
          throw;
        }
        // getEntryInformation is not available in this version of
        // Eden. Fall back to the older, more expensive
        // getFileInformation below.
      }
    }

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(
        info, std::string{rootPath.view()}, names, getSyncBehavior());
    applyResults(info);
  }

  void applyInformationOrError(const EntryInformationOrError& infoOrErr) {
    if (infoOrErr.getType() == EntryInformationOrError::Type::info) {
      dtype_ = getDTypeFromEden(*infoOrErr.get_info().dtype());
      setExists(true);
    } else {
      setExists(false);
    }
  }

  void applyInformationOrError(const FileInformationOrError& infoOrErr) {
    if (infoOrErr.getType() == FileInformationOrError::Type::info) {
      FileInformation stat;

      stat.size = *infoOrErr.get_info().size();
      stat.mode = *infoOrErr.get_info().mode();
      stat.mtime.tv_sec = *infoOrErr.get_info().mtime()->seconds();
      stat.mtime.tv_nsec = *infoOrErr.get_info().mtime()->nanoSeconds();

      stat_ = std::move(stat);
      setExists(true);
    } else {
      setExists(false);
    }
  }
};

static std::string escapeGlobSpecialChars(w_string_piece str) {
  std::string result;

  for (size_t i = 0; i < str.size(); ++i) {
    auto c = str[i];
    switch (c) {
      case '*':
      case '?':
      case '[':
      case ']':
      case '\\':
        result.append("\\");
        break;
    }
    result.append(&c, 1);
  }

  return result;
}

/** filter out paths that are ignored or that are not part of the
 * relative_root restriction in a query.
 * Ideally we'd pass this information into eden so that it doesn't
 * have to walk those paths and return the data to us, but for the
 * moment we have to filter it out of the results.
 * We need to respect the ignore_dirs configuration setting and
 * also remove anything that doesn't match the relative_root constraint
 * in the query. */
void filterOutPaths(std::vector<NameAndDType>& files, QueryContext* ctx) {
  files.erase(
      std::remove_if(
          files.begin(),
          files.end(),
          [ctx](const NameAndDType& item) {
            auto full = w_string::pathCat({ctx->root->root_path, item.name});

            if (!ctx->fileMatchesRelativeRoot(full)) {
              // Not in the desired area, so filter it out
              return true;
            }

            return ctx->root->ignore.isIgnored(full.data(), full.size());
          }),
      files.end());
}

void appendGlobResultToNameAndDTypeVec(
    std::vector<NameAndDType>& results,
    Glob&& glob) {
  size_t i = 0;
  size_t numDTypes = glob.get_dtypes().size();

  for (auto& name : glob.get_matchingFiles()) {
    // The server may not support dtypes, so this list may be empty.
    // This cast is OK because eden returns the system dependent bits to us, and
    // our DType enum is declared in terms of those bits
    auto dtype = i < numDTypes ? static_cast<DType>(glob.get_dtypes()[i])
                               : DType::Unknown;
    results.emplace_back(name, dtype);
    ++i;
  }
}

/** Returns the files that match the glob. */
std::vector<NameAndDType> globNameAndDType(
    StreamingEdenServiceAsyncClient* client,
    const std::string& mountPoint,
    const std::vector<std::string>& globPatterns,
    bool includeDotfiles,
    bool splitGlobPattern = false) {
  // TODO(xavierd): Once the config: "eden_split_glob_pattern" is rolled out
  // everywhere, remove this code.
  if (splitGlobPattern && globPatterns.size() > 1) {
    folly::DrivableExecutor* executor =
        folly::EventBaseManager::get()->getEventBase();

    std::vector<folly::Future<Glob>> globFutures;
    globFutures.reserve(globPatterns.size());
    for (const std::string& globPattern : globPatterns) {
      GlobParams params;
      params.mountPoint() = mountPoint;
      params.globs() = std::vector<std::string>{globPattern};
      params.includeDotfiles() = includeDotfiles;
      params.wantDtype() = true;
      params.sync() = getSyncBehavior();

      globFutures.emplace_back(
          client->semifuture_globFiles(params).via(executor));
    }

    std::vector<NameAndDType> allResults;
    for (folly::Future<Glob>& globFuture : globFutures) {
      appendGlobResultToNameAndDTypeVec(
          allResults, std::move(globFuture).getVia(executor));
    }
    return allResults;
  } else {
    GlobParams params;
    params.mountPoint() = mountPoint;
    params.globs() = globPatterns;
    params.includeDotfiles() = includeDotfiles;
    params.wantDtype() = true;
    params.sync() = getSyncBehavior();

    Glob glob;
    client->sync_globFiles(glob, params);
    std::vector<NameAndDType> result;
    appendGlobResultToNameAndDTypeVec(result, std::move(glob));
    return result;
  }
}

namespace {

/**
 * Construct a pooled Thrift channel that will automatically reconnect to
 * EdenFS on error.
 */
std::shared_ptr<apache::thrift::RequestChannel> makeThriftChannel(
    w_string rootPath,
    int numRetries) {
  auto channel = apache::thrift::PooledRequestChannel::newChannel(
      folly::EventBaseManager::get()->getEventBase(),
      folly::getUnsafeMutableGlobalIOExecutor(),
      [numRetries, rootPath = std::move(rootPath)](folly::EventBase& eb) {
        return apache::thrift::RetryingRequestChannel::newChannel(
            eb,
            numRetries,
            apache::thrift::ReconnectingRequestChannel::newChannel(
                eb, [rootPath](folly::EventBase& eb) {
                  return apache::thrift::RocketClientChannel::newChannel(
                      AsyncSocket::newSocket(
                          &eb, getEdenSocketAddress(rootPath)));
                }));
      });
  return channel;
}

} // namespace

class EdenView final : public QueryableView {
 public:
  explicit EdenView(const w_string& root_path, const Configuration& config)
      : QueryableView{root_path, /*requiresCrawl=*/false},
        rootPath_(root_path),
        thriftChannel_(makeThriftChannel(
            rootPath_,
            config.getInt("eden_retry_connection_count", 3))),
        mountPoint_(root_path.string()),
        splitGlobPattern_(config.getBool("eden_split_glob_pattern", false)),
        useStreamingSince_(config.getBool("eden_use_streaming_since", false)),
        thresholdForFreshInstance_(config.getInt(
            "eden_file_count_threshold_for_fresh_instance",
            10000)),
        filesChangedBetweenCommits_(
            Configuration(),
            "scm_hg_files_between_commits",
            32,
            10) {}

  void timeGenerator(const Query* /*query*/, QueryContext* ctx) const override {
    ctx->generationStarted();

    if (ctx->since.is_timestamp()) {
      throw QueryExecError(
          "timestamp based since queries are not supported with eden");
    }

    auto allFilesResult = getAllChangesSince(ctx);
    auto& resultPosition = allFilesResult.resultPosition;
    auto& fileInfo = allFilesResult.fileInfo;
    // We use the list of created files to synthesize the "new" field
    // in the file results
    auto& createdFileNames = allFilesResult.createdFileNames;

    // Filter out any ignored files
    filterOutPaths(fileInfo, ctx);

    auto isFreshInstance = ctx->since.is_fresh_instance();
    for (auto& item : fileInfo) {
      // a file is considered new if it was present in the created files
      // set returned from eden.
      bool isNew = createdFileNames.find(item.name) != createdFileNames.end();

      auto file = make_unique<EdenFileResult>(
          rootPath_,
          thriftChannel_,
          w_string::pathCat({mountPoint_, item.name}),
          &resultPosition,
          isNew,
          item.dtype);

      if (isFreshInstance) {
        // Fresh instance queries only return data about files
        // that currently exist, and we know this to be true
        // here because our list of files comes from evaluating
        // a glob.
        file->setExists(true);
      }

      w_query_process_file(ctx->query, ctx, std::move(file));
    }

    ctx->bumpNumWalked(fileInfo.size());
  }

  folly::SemiFuture<folly::Unit> waitForSettle(
      std::chrono::milliseconds /*settle_period*/) override {
    // We could implement this feature for EdenFS, but since the
    // Watchman-EdenFS integration is correct and waitForSettle is a workaround
    // for broken filesystem notification APIs, do nothing for now.
    return folly::unit;
  }

  CookieSync::SyncResult syncToNow(
      const std::shared_ptr<Root>& root,
      std::chrono::milliseconds timeout) override {
    try {
      return sync(root).get(timeout);
    } catch (const folly::FutureTimeout& ex) {
      throw std::system_error(ETIMEDOUT, std::generic_category(), ex.what());
    }
    return {};
  }

  folly::SemiFuture<CookieSync::SyncResult> sync(
      const std::shared_ptr<Root>&) override {
    return folly::makeSemiFutureWith([this]() {
             // Set an unlimited timeout. The caller is responsible for using a
             // timeout to bound the time spent in this method.
             facebook::eden::SyncBehavior sync;
             sync.syncTimeoutSeconds() = -1;

             facebook::eden::SynchronizeWorkingCopyParams params;
             params.sync() = sync;

             auto client = getEdenClient(thriftChannel_);
             return client->semifuture_synchronizeWorkingCopy(
                 mountPoint_, params);
           })
        .defer([](folly::Try<folly::Unit> try_) {
          if (try_.hasException()) {
            if (auto* exc =
                    try_.tryGetExceptionObject<TApplicationException>()) {
              if (exc->getType() == TApplicationException::UNKNOWN_METHOD) {
                return folly::Try{CookieSync::SyncResult{}};
              }
            }
            return folly::Try<CookieSync::SyncResult>{
                std::move(try_.exception())};
          }
          return folly::Try{CookieSync::SyncResult{}};
        });
  }

  void executeGlobBasedQuery(
      const std::vector<std::string>& globStrings,
      const Query* query,
      QueryContext* ctx,
      bool includeDir = true) const {
    auto client = getEdenClient(thriftChannel_);

    auto includeDotfiles = (query->glob_flags & WM_PERIOD) == 0;
    auto fileInfo = globNameAndDType(
        client.get(),
        mountPoint_,
        globStrings,
        includeDotfiles,
        splitGlobPattern_);

    // Filter out any ignored files
    filterOutPaths(fileInfo, ctx);

    for (auto& item : fileInfo) {
      auto file = make_unique<EdenFileResult>(
          rootPath_,
          thriftChannel_,
          w_string::pathCat({mountPoint_, item.name}),
          /* position=*/nullptr,
          /*isNew=*/false,
          item.dtype);

      // The results of a glob are known to exist
      file->setExists(true);

      // Skip processing directories
      if (!includeDir && item.dtype == DType::Dir) {
        continue;
      }

      w_query_process_file(ctx->query, ctx, std::move(file));
    }

    ctx->bumpNumWalked(fileInfo.size());
  }

  // Helper for computing a relative path prefix piece.
  // The returned piece is owned by the supplied context object!
  w_string_piece computeRelativePathPiece(QueryContext* ctx) const {
    w_string_piece rel;
    if (ctx->query->relative_root) {
      rel = ctx->query->relative_root;
      rel.advance(ctx->root->root_path.size() + 1);
    }
    return rel;
  }

  /** Walks files that match the supplied set of paths */
  void pathGenerator(const Query* query, QueryContext* ctx) const override {
    ctx->generationStarted();
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    auto rel = computeRelativePathPiece(ctx);

    std::vector<std::string> globStrings;
    // Translate the path list into a list of globs
    for (auto& path : *query->paths) {
      if (path.depth > 0) {
        // We don't have an easy way to express depth constraints
        // in the existing glob API, so we just punt for the moment.
        // I believe that this sort of query is quite rare anyway.
        throw QueryExecError(
            "the eden watcher only supports depth 0 or depth -1");
      }
      // -1 depth is infinite which we can translate to a recursive
      // glob.  0 depth is direct descendant which we can translate
      // to a simple * wildcard.
      auto glob = path.depth == -1 ? "**/*" : "*";

      globStrings.emplace_back(std::string{
          w_string::pathCat({rel, escapeGlobSpecialChars(path.name), glob})
              .view()});
    }
    executeGlobBasedQuery(globStrings, query, ctx);

    // We send another round of glob queries to query about the information
    // about the path themselves since we want to include the paths if they are
    // files.
    // TODO(zeyi): replace this with builtin path generator inside EdenFS
    globStrings.clear();
    for (auto& path : *query->paths) {
      globStrings.emplace_back(std::string{
          w_string::pathCat({rel, escapeGlobSpecialChars(path.name)}).view()});
    }

    executeGlobBasedQuery(globStrings, query, ctx, false);
  }

  void globGenerator(const Query* query, QueryContext* ctx) const override {
    if (!query->glob_tree) {
      // If we are called via the codepath in the query evaluator that
      // just speculatively executes queries then `glob` may not be
      // present; short-circuit in that case.
      return;
    }

    ctx->generationStarted();
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    auto rel = computeRelativePathPiece(ctx);

    std::vector<std::string> globStrings;
    for (auto& glob : query->glob_tree->unparse()) {
      globStrings.emplace_back(
          std::string{w_string::pathCat({rel, glob}).view()});
    }

    // More glob flags/functionality:
    auto noescape = bool(query->glob_flags & WM_NOESCAPE);
    if (noescape) {
      throw QueryExecError(
          "glob_noescape is not supported for the eden watcher");
    }
    executeGlobBasedQuery(globStrings, query, ctx);
  }

  void allFilesGenerator(const Query* query, QueryContext* ctx) const override {
    ctx->generationStarted();
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    std::string globPattern;
    auto rel = computeRelativePathPiece(ctx);
    if (rel.size() > 0) {
      globPattern.append(rel.data(), rel.size());
      globPattern.append("/");
    }
    globPattern.append("**");
    executeGlobBasedQuery(std::vector<std::string>{globPattern}, query, ctx);
  }

  ClockPosition getMostRecentRootNumberAndTickValue() const override {
    auto client = getEdenClient(thriftChannel_);
    JournalPosition position;
    client->sync_getCurrentJournalPosition(position, mountPoint_);
    return ClockPosition(
        *position.mountGeneration(), *position.sequenceNumber());
  }

  w_string getCurrentClockString() const override {
    return getMostRecentRootNumberAndTickValue().toClockString();
  }

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& /*fileNames*/) const override {
    return false;
  }

  void startThreads(const std::shared_ptr<Root>& root) override {
    auto self = shared_from_this();
    std::thread thr([self, this, root]() { subscriberThread(root); });
    thr.detach();
  }

  void stopThreads() override {
    subscriberEventBase_.terminateLoopSoon();
  }

  json_ref getWatcherDebugInfo() const override {
    return json_null();
  }

  void clearWatcherDebugInfo() override {}

  using EdenFSSubcription =
      apache::thrift::ClientBufferedStream<JournalPosition>::Subscription;

  EdenFSSubcription rocketSubscribe(
      std::shared_ptr<Root> root,
      SettleCallback& settleCallback,
      GetJournalPositionCallback& getJournalPositionCallback,
      std::chrono::milliseconds settleTimeout) {
    auto client = getEdenClient(thriftChannel_);
    auto stream = client->sync_subscribeStreamTemporary(
        std::string(root->root_path.data(), root->root_path.size()));
    return std::move(stream).subscribeExTry(
        &subscriberEventBase_,
        [&settleCallback,
         &getJournalPositionCallback,
         this,
         root,
         settleTimeout](folly::Try<JournalPosition>&& t) {
          if (t.hasValue()) {
            try {
              log(DBG, "Got subscription push from eden\n");
              if (settleCallback.isScheduled()) {
                log(DBG, "reschedule settle timeout\n");
                settleCallback.cancelTimeout();
              }
              subscriberEventBase_.timer().scheduleTimeout(
                  &settleCallback, settleTimeout);

              // For bursty writes to the working copy, let's limit the
              // amount of notification that Watchman receives by
              // scheduling a getCurrentJournalPosition call in the future.
              //
              // Thus, we're guarantee to only receive one notification per
              // settleTimeout/2 and no more, regardless of how much
              // writing is done in the repository.
              subscriberEventBase_.timer().scheduleTimeout(
                  &getJournalPositionCallback, settleTimeout / 2);
            } catch (const std::exception& exc) {
              log(ERR,
                  "Exception while processing eden subscription: ",
                  exc.what(),
                  ": cancel watch\n");
              subscriberEventBase_.terminateLoopSoon();
            }
          } else {
            auto reason = t.hasException()
                ? folly::exceptionStr(std::move(t.exception()))
                : "controlled shutdown";
            log(ERR,
                "subscription stream ended: ",
                w_string_piece(reason.data(), reason.size()),
                ", cancel watch\n");
            // We won't be called again, but we terminate the loop
            // just to make sure.
            subscriberEventBase_.terminateLoopSoon();
          }
        });
  }

  // This is the thread that we use to listen to the stream of
  // changes coming in from the EdenFS server.
  void subscriberThread(std::shared_ptr<Root> root) noexcept {
    SCOPE_EXIT {
      // ensure that the root gets torn down,
      // otherwise we'd leave it in a broken state.
      root->cancel();
    };

    w_set_thread_name("edensub ", root->root_path.view());
    log(DBG, "Started subscription thread\n");

    std::optional<EdenFSSubcription> subscription;
    SCOPE_EXIT {
      if (subscription.has_value()) {
        subscription->cancel();
        std::move(*subscription).join();
      }
    };

    try {
      // Prepare the callback
      SettleCallback settleCallback{&subscriberEventBase_, root};
      GetJournalPositionCallback getJournalPositionCallback{
          &subscriberEventBase_, thriftChannel_, mountPoint_};
      // Figure out the correct value for settling
      std::chrono::milliseconds settleTimeout(root->trigger_settle);

      subscription = rocketSubscribe(
          root, settleCallback, getJournalPositionCallback, settleTimeout);

      // This will run until the stream ends
      log(DBG, "Started subscription thread loop\n");
      subscribeReadyPromise_.setValue();
      subscriberEventBase_.loop();

    } catch (const std::exception& exc) {
      log(ERR,
          "uncaught exception in subscription thread, cancel watch:",
          exc.what(),
          "\n");
    }
  }

  const w_string& getName() const override {
    static w_string name("eden");
    return name;
  }

  folly::SemiFuture<folly::Unit> waitUntilReadyToQuery() override {
    return subscribeReadyPromise_.getSemiFuture();
  }

 private:
  /**
   * Returns all the files in the watched directory for a fresh instance.
   *
   * In the case where the query specifically ask for an empty file list on a
   * fresh instance, an empty vector will be returned.
   */
  std::vector<NameAndDType> getAllFilesForFreshInstance(
      QueryContext* ctx) const {
    if (ctx->query->empty_on_fresh_instance) {
      // Avoid a full tree walk if we don't need it!
      return std::vector<NameAndDType>();
    }

    std::string globPattern;
    if (ctx->query->relative_root) {
      w_string_piece rel(ctx->query->relative_root);
      rel.advance(ctx->root->root_path.size() + 1);
      globPattern.append(rel.data(), rel.size());
      globPattern.append("/");
    }
    globPattern.append("**");

    auto includeDotfiles = (ctx->query->glob_flags & WM_PERIOD) == 0;

    auto client = getEdenClient(thriftChannel_);
    return globNameAndDType(
        client.get(),
        mountPoint_,
        std::vector<std::string>{std::move(globPattern)},
        includeDotfiles);
  }

  struct GetAllChangesSinceResult {
    JournalPosition resultPosition;
    std::vector<NameAndDType> fileInfo;
    std::unordered_set<std::string> createdFileNames;
  };

  /**
   * Build a GetAllChangesSinceResult for a fresh instance.
   */
  GetAllChangesSinceResult makeFreshInstance(QueryContext* ctx) const {
    GetAllChangesSinceResult result;

    ctx->since.set_fresh_instance();
    getEdenClient(thriftChannel_)
        ->sync_getCurrentJournalPosition(result.resultPosition, mountPoint_);
    result.fileInfo = getAllFilesForFreshInstance(ctx);

    return result;
  }

  /**
   * Compute all the changes by querying the getFilesChangedSince API.
   *
   * This will first compute all the change and then add them to the
   * GetAllChangesSinceResult::fileInfo vector.
   *
   * On error, or when thresholdForFreshInstance_ is exceeded, the clock will
   * be modified to indicate a fresh instance and an empty set of files will be
   * returned.
   */
  GetAllChangesSinceResult getAllChangesSinceLegacy(QueryContext* ctx) const {
    auto client = getEdenClient(thriftChannel_);

    // Query eden to fill in the mountGeneration field.
    JournalPosition position;
    client->sync_getCurrentJournalPosition(position, mountPoint_);
    // dial back to the sequence number from the query
    *position.sequenceNumber() =
        std::get<QuerySince::Clock>(ctx->since.since).ticks;

    GetAllChangesSinceResult result;

    // Now we can get the change journal from eden
    FileDelta delta;
    client->sync_getFilesChangedSince(delta, mountPoint_, position);

    result.createdFileNames.insert(
        delta.createdPaths()->begin(), delta.createdPaths()->end());

    // The list of changed files is the union of the created, added,
    // and removed sets returned from eden in list form.
    for (auto& name : *delta.changedPaths()) {
      result.fileInfo.emplace_back(std::move(name));
    }
    for (auto& name : *delta.removedPaths()) {
      result.fileInfo.emplace_back(std::move(name));
    }
    for (auto& name : *delta.createdPaths()) {
      result.fileInfo.emplace_back(std::move(name));
    }

    bool didChangeCommits = delta.snapshotTransitions()->size() >= 2 ||
        (delta.fromPosition()->snapshotHash() !=
         delta.toPosition()->snapshotHash());

    if (didChangeCommits && getSCM()) {
      // Check whether they checked out a new commit or reset the commit to
      // a different hash.  We interrogate source control to discover
      // the set of changed files between those hashes, and then
      // add in any paths that may have changed around snapshot hash
      // changes events;  These are files whose status cannot be
      // determined purely from source control operations.

      std::unordered_set<std::string> mergedFileList;
      for (auto& info : result.fileInfo) {
        mergedFileList.insert(info.name);
      }

      SCM::StatusResult changedBetweenCommits;
      if (delta.snapshotTransitions()->empty()) {
        auto fromHash = folly::hexlify(*delta.fromPosition()->snapshotHash());
        auto toHash = folly::hexlify(*delta.toPosition()->snapshotHash());

        // Legacy path: this (incorrectly) ignores any commit transitions
        // between the initial commit hash and the final commit hash.
        log(ERR,
            "since ",
            *position.sequenceNumber(),
            " we changed commit hashes from ",
            fromHash,
            " to ",
            toHash,
            "\n");

        std::vector<std::string> commits{
            std::move(fromHash), std::move(toHash)};
        changedBetweenCommits = getFilesChangedBetweenCommits(
            std::move(commits), ctx->query->alwaysIncludeDirectories);
      } else {
        std::vector<std::string> commits;
        commits.reserve(delta.snapshotTransitions()->size());
        for (auto& hash : *delta.snapshotTransitions()) {
          commits.push_back(folly::hexlify(hash));
        }
        log(ERR,
            "since ",
            *position.sequenceNumber(),
            " we changed commit hashes ",
            folly::join(" -> ", commits),
            "\n");
        changedBetweenCommits = getFilesChangedBetweenCommits(
            std::move(commits), ctx->query->alwaysIncludeDirectories);
      }

      for (auto& fileName : changedBetweenCommits.changedFiles) {
        mergedFileList.insert(std::string{fileName.view()});
      }
      for (auto& fileName : changedBetweenCommits.removedFiles) {
        mergedFileList.insert(std::string{fileName.view()});
      }
      for (auto& fileName : changedBetweenCommits.addedFiles) {
        mergedFileList.insert(std::string{fileName.view()});
        result.createdFileNames.insert(std::string{fileName.view()});
      }

      // Engineers usually don't work on a thousands of files, but on an
      // giant monorepo, the set of files changed in between 2 revisions
      // can be very large, and continuing down this route would force
      // Watchman to fetch metadata about a ton of files, causing delay in
      // answering the query and large amount of network traffic.
      //
      // On these monorepos, tools also set the empty_on_fresh_instance
      // flag, thus we can simply pretend to return a fresh instance and an
      // empty fileInfo list.
      if (thresholdForFreshInstance_ != 0 &&
          mergedFileList.size() > thresholdForFreshInstance_ &&
          ctx->query->empty_on_fresh_instance) {
        log(DBG,
            "Pretending to be a fresh instance due to too many files changed: ",
            mergedFileList.size(),
            "\n");
        ctx->since.set_fresh_instance();
        result.fileInfo.clear();
      } else {
        // We don't know whether the unclean paths are added, removed
        // or just changed.  We're going to treat them as changed.
        mergedFileList.insert(
            std::make_move_iterator(delta.uncleanPaths()->begin()),
            std::make_move_iterator(delta.uncleanPaths()->end()));

        // Replace the list of fileNames with the de-duped set
        // of names we've extracted from source control
        result.fileInfo.clear();
        for (auto name : mergedFileList) {
          result.fileInfo.emplace_back(std::move(name));
        }
      }
    }

    result.resultPosition = *delta.toPosition();
    log(DBG,
        "wanted from ",
        *position.sequenceNumber(),
        " result delta from ",
        *delta.fromPosition()->sequenceNumber(),
        " to ",
        *delta.toPosition()->sequenceNumber(),
        " with ",
        result.fileInfo.size(),
        " changed files\n");
    return result;
  }

  GetAllChangesSinceResult getAllChangesSinceStreaming(
      QueryContext* ctx) const {
    auto client = getEdenClient(thriftChannel_);

    // Query eden to fill in the mountGeneration field.
    auto position =
        client->semifuture_getCurrentJournalPosition(mountPoint_).get();
    // dial back to the sequence number from the query
    *position.sequenceNumber() =
        std::get<QuerySince::Clock>(ctx->since.since).ticks;

    StreamChangesSinceParams params;
    params.mountPoint() = mountPoint_;
    params.fromPosition() = position;
    auto [resultChangesSince, stream] = client->sync_streamChangesSince(params);

    GetAllChangesSinceResult result;
    result.resultPosition = *resultChangesSince.toPosition();

    // -1 = removed
    // 0 = changed
    // 1 = added
    std::unordered_map<std::string, int> byFile;
    std::unordered_map<std::string, EdenDtype> dtypes;

    std::move(stream).subscribeInline(
        [&](folly::Try<ChangedFileResult>&& changeTry) mutable {
          if (changeTry.hasException()) {
            log(ERR,
                "Error: ",
                folly::exceptionStr(changeTry.exception()),
                "\n");
            result = makeFreshInstance(ctx);
            return false;
          }

          if (!changeTry.hasValue()) {
            // End of the stream.
            return false;
          }

          const auto& change = changeTry.value();
          auto& name = *change.name();

          // Changes needs to be deduplicated so a file that was added and then
          // removed is reported as MODIFIED.
          switch (*change.status()) {
            case ScmFileStatus::ADDED:
              byFile[name] += 1;
              break;
            case ScmFileStatus::MODIFIED:
              byFile[name];
              break;
            case ScmFileStatus::REMOVED:
              byFile[name] -= 1;
              break;
            case ScmFileStatus::IGNORED:
              break;
          }

          auto dtype = *change.dtype();
          auto [element, inserted] = dtypes.emplace(name, dtype);
          if (!inserted && element->second != dtype) {
            // Due to streamChangesSince not providing any ordering guarantee,
            // Watchman can't tell what DType a file has in the case where it
            // changed. Thus let's fallback to an UNKNOWN type, and Watchman
            // will later query the actual DType from EdenFS.
            element->second = EdenDtype::UNKNOWN;
          }

          // Engineers usually don't work on a thousands of files, but on an
          // giant monorepo, the set of files changed in between 2 revisions
          // can be very large, and continuing down this route would force
          // Watchman to fetch metadata about a ton of files, causing delay in
          // answering the query and large amount of network traffic.
          //
          // On these monorepos, tools also set the empty_on_fresh_instance
          // flag, thus we can simply pretend to return a fresh instance and an
          // empty fileInfo list.
          if (thresholdForFreshInstance_ != 0 &&
              byFile.size() > thresholdForFreshInstance_ &&
              ctx->query->empty_on_fresh_instance) {
            result = makeFreshInstance(ctx);
            return false;
          }

          return true;
        });

    for (auto& [name, count] : byFile) {
      result.fileInfo.emplace_back(name, getDTypeFromEden(dtypes[name]));
      if (count > 0) {
        result.createdFileNames.emplace(name);
      }
    }

    return result;
  }

  /**
   * Compute and return all the changes that occured since the last call.
   *
   * On error, or when thresholdForFreshInstance_ is exceeded, the clock will
   * be modified to indicate a fresh instance and an empty set of files will be
   * returned.
   */
  GetAllChangesSinceResult getAllChangesSince(QueryContext* ctx) const {
    if (ctx->since.is_fresh_instance()) {
      // Earlier in the processing flow, we decided that the rootNumber
      // didn't match the current root which means that eden was restarted.
      // We need to translate this to a fresh instance result set and
      // return a list of all possible matching files.
      return makeFreshInstance(ctx);
    }

    try {
      if (!useStreamingSince_) {
        return getAllChangesSinceLegacy(ctx);
      } else {
        return getAllChangesSinceStreaming(ctx);
      }
    } catch (const EdenError& err) {
      // ERANGE: mountGeneration differs
      // EDOM: journal was truncated.
      // For other situations we let the error propagate.
      XCHECK(err.errorCode_ref());
      if (*err.errorCode_ref() != ERANGE && *err.errorCode_ref() != EDOM) {
        throw;
      }
      // mountGeneration differs, or journal was truncated,
      // so treat this as equivalent to a fresh instance result
      return makeFreshInstance(ctx);
    } catch (const SCMError& err) {
      // Most likely this means a checkout occurred but we encountered
      // an error trying to get the list of files changed between the two
      // commits.  Generate a fresh instance result since we were unable
      // to compute the list of files changed.
      log(ERR,
          "SCM error while processing EdenFS journal update: ",
          err.what(),
          "\n");
      return makeFreshInstance(ctx);
    }
  }

  // Compute the set of paths that have changed across all of the transitions
  // between the list of given commits.
  //
  // For example, if commits is [A, B, C], then this accumulates the changes
  // between [A, B] and [B, C] into one StatusResult.
  //
  // This is purely a history operation and does not consider the working copy
  // status.
  //
  // This will  also report all directories changed.
  SCM::StatusResult getFilesChangedBetweenCommits(
      std::vector<std::string> commits,
      bool alwaysIncludeDirectories) const {
    if (alwaysIncludeDirectories) {
      // Very few clients are passing this, let's pessimize them by falling
      // back to the SCM path.
      //
      // TODO(xavierd): once EdenFS starts providing the set of files changed
      // between journal clocks these clients will be fixed for good.
      return getSCM()->getFilesChangedBetweenCommits(
          std::move(commits), nullptr, alwaysIncludeDirectories);
    }

    auto client = getEdenClient(thriftChannel_);

    // -1 = removed
    // 0 = changed
    // 1 = added
    std::unordered_map<std::string, int> byFile;

    for (auto i = 0ul; i + 1 < commits.size(); i++) {
      auto& commitFrom = commits[i];
      auto& commitTo = commits[i + 1];

      if (commitFrom == commitTo) {
        // Older versions of EdenFS could report "commit transitions" from A to
        // A, in which case we shouldn't ask for the difference.
        continue;
      }

      auto key = fmt::format("{}:{}", commitFrom, commitTo);
      auto status =
          filesChangedBetweenCommits_
              .get(
                  key,
                  [&](auto&&) -> folly::Future<ScmStatus> {
                    // Note, we can't use future_getScmStatusBetweenRevisions
                    // here as the .get() below will drive a secondary future
                    // that LRUCache creates and not the one returned by this
                    // lambda. Thus we need to make sure the SemiFuture is
                    // running on an executor.
                    return client
                        ->semifuture_getScmStatusBetweenRevisions(
                            std::string{rootPath_.view()}, commitFrom, commitTo)
                        .via(&getThreadPool());
                  })
              // TODO(xavierd): rate limit in EdenFS so all the
              // getScmStatusBetweenRevisions calls can be sent to it
              // concurrently instead of sequentially.
              .get()
              ->value();

      if (!status.errors()->empty()) {
        SCMError::throwf(
            "Failed to get status betwen {} and {}", commitFrom, commitTo);
      }

      for (const auto& [name, file_status] : *status.entries()) {
        switch (file_status) {
          case ScmFileStatus::ADDED:
            byFile[name] += 1;
            break;
          case ScmFileStatus::MODIFIED:
            byFile[name];
            break;
          case ScmFileStatus::REMOVED:
            byFile[name] -= 1;
            break;
          default:
            break;
        }
      }
    }

    SCM::StatusResult res;

    for (auto& [name, count] : byFile) {
      if (count > 0) {
        res.addedFiles.emplace_back(name);
      } else if (count == 0) {
        res.changedFiles.emplace_back(name);
        continue;
      } else {
        res.removedFiles.emplace_back(name);
      }
    }

    return res;
  }

  w_string rootPath_;
  std::shared_ptr<apache::thrift::RequestChannel> thriftChannel_;
  folly::EventBase subscriberEventBase_;
  std::string mountPoint_;
  folly::SharedPromise<folly::Unit> subscribeReadyPromise_;
  bool splitGlobPattern_;
  bool useStreamingSince_;
  unsigned int thresholdForFreshInstance_;

  mutable LRUCache<std::string, ScmStatus> filesChangedBetweenCommits_;
};

#ifdef _WIN32
// Test if EdenFS is stopped for the given path.
bool isEdenStopped(w_string root) {
  static const w_string_piece kStar{"*"};
  static const w_string_piece kNonExistencePath{"EDEN_TEST_NON_EXISTENCE_PATH"};
  auto queryRaw = w_string::pathCat({root, kNonExistencePath, kStar});
  auto query = queryRaw.normalizeSeparators();
  std::wstring wquery = query.piece().asWideUNC();
  WIN32_FIND_DATAW ffd;

  auto find = FindFirstFileW(wquery.c_str(), &ffd);
  SCOPE_EXIT {
    if (find != INVALID_HANDLE_VALUE) {
      FindClose(find);
    }
  };

  auto lastError = GetLastError();

  // When EdenFS is not running, `FindFirstFile` will fail with this error
  // since it can't reach EdenFS to query directory information.
  if (find == INVALID_HANDLE_VALUE &&
      lastError == ERROR_FILE_SYSTEM_VIRTUALIZATION_UNAVAILABLE) {
    log(DBG, "edenfs is NOT RUNNING\n");
    return true;
  }

  log(DBG, "edenfs is RUNNING\n");
  return false;
}

constexpr ULONG REPARSE_TAG_GVFS = 0x9000001c;

w_string findEdenFSRoot(w_string_piece root_path) {
  w_string_piece path = root_path;
  w_string_piece result = nullptr;
  while (true) {
    auto fd =
        openFileHandle(path.data(), OpenFileHandleOptions::queryFileInfo());

    // projected fs reparse tag
    if (fd.getReparseTag() == REPARSE_TAG_GVFS) {
      result = path;
    } else {
      // Otherwise we are out of reparse tree, exit.
      break;
    }

    auto next = path.dirName();
    if (next == path) {
      // We can't go any higher, so we couldn't find the
      // requested path(s)
      break;
    }

    path = next;
  }

  return result.asWString();
}
#endif

std::shared_ptr<QueryableView> detectEden(
    const w_string& root_path,
    const w_string& fstype,
    const Configuration& config) {
#ifdef _WIN32
  auto edenRoot = findEdenFSRoot(root_path);
  if (edenRoot) {
    if (isEdenStopped(root_path)) {
      throw TerminalWatcherError(to<std::string>(
          root_path.view(),
          " appears to be an offline EdenFS mount. "
          "Try running `edenfsctl start` to bring it back online and "
          "then retry your watch"));
    }

    try {
      return std::make_shared<EdenView>(root_path, config);
    } catch (const std::exception& exc) {
      throw TerminalWatcherError(to<std::string>(
          "Failed to initialize eden watcher, and since this is an Eden "
          "repo, will not allow falling back to another watcher.  Error was: ",
          exc.what()));
    }
  }

  throw std::runtime_error(
      to<std::string>("Not an Eden clone: ", root_path.view()));

#else
  if (!is_edenfs_fs_type(fstype) && fstype != "fuse" &&
      fstype != "osxfuse_eden" && fstype != "macfuse_eden" &&
      fstype != "edenfs_eden") {
    // Not an active EdenFS mount.  Perhaps it isn't mounted yet?
    auto readme = to<std::string>(root_path.view(), "/README_EDEN.txt");
    try {
      (void)getFileInformation(readme.c_str());
    } catch (const std::exception&) {
      // We don't really care if the readme doesn't exist or is inaccessible,
      // we just wanted to do a best effort check for the readme file.
      // If we can't access it, we're still not in a position to treat
      // this as an EdenFS mount so record the issue and allow falling
      // back to one of the other watchers.
      throw std::runtime_error(
          to<std::string>(fstype.view(), " is not a FUSE file system"));
    }

    // If we get here, then the readme file/symlink exists.
    // If the readme exists then this is an offline eden mount.
    // We can't watch it using this watcher in its current state,
    // and we don't want to allow falling back to inotify as that
    // will be horribly slow.
    throw TerminalWatcherError(to<std::string>(
        root_path.view(),
        " appears to be an offline EdenFS mount. "
        "Try running `eden doctor` to bring it back online and "
        "then retry your watch"));
  }

  auto edenRoot = readSymbolicLink(
      to<std::string>(root_path.view(), "/.eden/root").c_str());
  if (edenRoot != root_path) {
    // We aren't at the root of the eden mount.
    // Throw a TerminalWatcherError to indicate that the Eden watcher is the
    // correct watcher type for this directory (so don't try other watcher
    // types), but that it can't be used due to an error.
    throw TerminalWatcherError(to<std::string>(
        "you may only watch from the root of an eden mount point. "
        "Try again using ",
        edenRoot.view()));
  }
#endif
  // Given that the readlink() succeeded, assume this is an Eden mount.
  return std::make_shared<EdenView>(root_path, config);
}

} // namespace

static WatcherRegistry
    reg("eden", detectEden, 100 /* prefer eden above others */);
} // namespace watchman
