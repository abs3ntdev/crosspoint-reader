#pragma once
#include <Epub.h>
#include <GfxRenderer.h>

#include <cstdint>
#include <memory>
#include <string>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

// Non-UI progress sync shared by KOReaderSyncActivity and Sync on Sleep.
namespace koreaderSync {

struct Session {
  std::string epubPath;
  CrossPointPosition localPosition{};
  SavedProgressPosition localProgress{};  // computed while the reader still had the book open
  std::string documentHash;
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress{};
  CrossPointPosition remotePosition{};
  std::shared_ptr<Epub> epub;  // loaded on demand; null while the network needs the heap
  const char* error = "";      // reason when a step fails

  void ensureEpubLoaded();
};

enum class Decision : uint8_t { Failed, NoRemote, Synchronized, LocalAhead, RemoteAhead, NeedsChoice };

// Step messages for the caller's UI (screen or popup).
using StatusFn = void (*)(const char* message, void* user);

// Hash, clipping sync (when enabled), fetch, map and compare. Smart sync also
// checks the alternate document id and decides; otherwise any remote record
// returns NeedsChoice. The caller must hold no render lock.
Decision fetchAndCompare(Session& session, GfxRenderer& renderer, bool smart, StatusFn status, void* user);

// Uploads local progress (rich position and metadata per server settings).
KOReaderSyncClient::Error upload(Session& session);

// Writes the remote position as the book's saved progress.
bool applyRemote(Session& session);

}  // namespace koreaderSync

// Sync on Sleep: the reader captures in prepareForSleep, the sleep path runs
// the Smart sync once WiFi is up, or skips with a reason.
namespace sleepSync {
void capture(std::shared_ptr<Epub> epub, const CrossPointPosition& position, const SavedProgressPosition& saved);
bool pending();
// Full Smart sync with step and result popups. WiFi must be connected.
void run(GfxRenderer& renderer);
// Drops a pending capture; shows reason as the result when one was pending.
void skip(GfxRenderer& renderer, const char* reason);
}  // namespace sleepSync
