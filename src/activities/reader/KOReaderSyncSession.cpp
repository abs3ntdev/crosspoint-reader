#include "KOReaderSyncSession.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <optional>
#include <utility>

#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "ProgressComparison.h"
#include "activities/RenderLock.h"
#include "clippings/ClippingSync.h"
#include "components/UITheme.h"
#include "network/WifiPowerSaveGuard.h"

namespace koreaderSync {
namespace {
std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

KOReaderProgress buildUpload(Session& s) {
  KOReaderProgress progress{};
  progress.document = s.documentHash;
  progress.progress = s.localProgress.xpath;
  progress.percentage = s.localProgress.percentage;

  // The HTTP client enforces the same server-type boundary before serializing.
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition pos;
    const float pct = s.localProgress.percentage < 0.0f   ? 0.0f
                      : s.localProgress.percentage > 1.0f ? 1.0f
                                                          : s.localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(s.localPosition.spineIndex);
    pos.pageNumber = static_cast<uint16_t>(s.localPosition.pageNumber);
    pos.totalPages = static_cast<uint16_t>(s.localPosition.totalPages > 0 ? s.localPosition.totalPages : 1);
    if (s.localPosition.hasParagraphIndex) pos.paragraphIndex = s.localPosition.paragraphIndex;
    pos.xpath = s.localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306).
  if (KOREADER_STORE.getSendMetadata()) {
    s.ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = s.epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? s.epubPath.substr(lastSlash + 1) : s.epubPath;
    if (s.epub) {
      meta.title = s.epub->getTitle();
      meta.authors = s.epub->getAuthor();
      if (KOREADER_STORE.supportsExtendedMetadata()) {
        Epub::SyncMetadata syncMetadata;
        if (s.epub->loadSyncMetadata(syncMetadata)) {
          meta.isbn = std::move(syncMetadata.isbn);
          meta.asin = std::move(syncMetadata.asin);
          meta.series = std::move(syncMetadata.series);
          meta.seriesIndex = syncMetadata.seriesIndex;
        } else {
          LOG_DBG("KOSync", "Could not read extended EPUB metadata; sending core metadata only");
        }
      }
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    // Plugin sidecar fields let a custom server route progress by a service
    // book id. A missing sidecar leaves extraJson empty.
    Storage.readFileToString("KOSync", s.epubPath + ".meta.json", 2 * 1024, meta.extraJson);
    progress.metadata = std::move(meta);
  }
  return progress;
}
}  // namespace

void Session::ensureEpubLoaded() {
  if (epub) return;
  LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS needed for progress mapping, and no rebuild if the cache is missing.
  if (!epub->load(false, true)) {
    LOG_ERR("KOSync", "Failed to load epub for progress mapping");
    epub.reset();
    return;
  }
  LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
}

Decision fetchAndCompare(Session& s, GfxRenderer& renderer, const bool smart, const StatusFn status, void* user) {
  WifiPowerSaveGuard psGuard;
  status(tr(STR_CALC_HASH), user);
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  s.documentHash = calculateDocumentHashForMethod(s.epubPath, primaryMethod);
  if (s.documentHash.empty()) {
    s.error = tr(STR_HASH_FAILED);
    return Decision::Failed;
  }
  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(primaryMethod), s.documentHash.c_str());

  if (KOREADER_STORE.getSyncClippings()) {
    status(tr(STR_SYNCING_CLIPPINGS), user);
    bool clippingsSynced;
    {
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
      RenderLock lock;
      GfxRenderer::FrameBufferLoan loan(renderer);
#endif
      clippingsSynced = clippingSync::run(s.epubPath, s.documentHash);
    }
    if (!clippingsSynced) {
      s.error = tr(STR_CLIPPING_SYNC_FAILED);
      return Decision::Failed;
    }
  }

  status(tr(STR_FETCH_PROGRESS), user);
  // Smart sync also keeps the alternate document-id record until both can be mapped.
  auto result = KOReaderSyncClient::getProgress(s.documentHash, s.remoteProgress);
  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
          matchMethodName(primaryMethod), result, KOReaderSyncClient::lastHttpCode, s.documentHash.c_str(),
          s.localProgress.percentage, s.remoteProgress.percentage, s.remoteProgress.progress.c_str());

  KOReaderProgress alternateProgress;
  bool hasAlternateProgress = false;
  if (smart) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(s.epubPath, altMethod);
    if (!altHash.empty() && altHash != s.documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
              matchMethodName(altMethod), altResult, KOReaderSyncClient::lastHttpCode, altHash.c_str(),
              s.localProgress.percentage, altProgress.percentage, altProgress.progress.c_str());
      if (altResult == KOReaderSyncClient::OK) {
        alternateProgress = std::move(altProgress);
        hasAlternateProgress = true;
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND && hasAlternateProgress) {
    s.remoteProgress = std::move(alternateProgress);
    hasAlternateProgress = false;
    result = KOReaderSyncClient::OK;
  }
  if (result == KOReaderSyncClient::NOT_FOUND) {
    s.hasRemoteProgress = false;
    return Decision::NoRemote;
  }
  if (result != KOReaderSyncClient::OK) {
    s.error = KOReaderSyncClient::errorString(result);
    return Decision::Failed;
  }

  // The Epub was released before the network calls to free heap for TLS; reload it to map.
  s.hasRemoteProgress = true;
  s.ensureEpubLoaded();
  if (!s.epub) {
    s.error = "";
    return Decision::Failed;
  }

  {
    RenderLock lock;
    GfxRenderer::FrameBufferLoan loan(renderer);

    const auto mapRemoteProgress = [&](const KOReaderProgress& progress) {
      // The standard KOReader progress XPath is the authoritative content anchor.
      // The CrossPoint server's rich page hints remain a legacy fallback.
      const SavedProgressPosition koPos = {progress.progress, progress.percentage};
      CrossPointPosition mapped =
          ProgressMapper::toCrossPoint(s.epub, koPos, renderer, s.localPosition.spineIndex, s.localPosition.totalPages);
      if (!mapped.hasVisibleTextOffset && progress.position.has_value()) {
        // toCrossPoint already tried koPos.xpath; skip re-resolving the same XPath.
        const bool sameXPath = progress.position->xpath == progress.progress;
        if (const auto richMapped = ProgressMapper::fromRichPosition(s.epub, *progress.position, renderer, sameXPath)) {
          mapped = *richMapped;
        }
      }
      return mapped;
    };

    s.remotePosition = mapRemoteProgress(s.remoteProgress);
    if (hasAlternateProgress) {
      const CrossPointPosition alternatePosition = mapRemoteProgress(alternateProgress);
      if (selectRemoteRecord(s.remotePosition, s.remoteProgress.percentage, alternatePosition,
                             alternateProgress.percentage) == RemoteRecordChoice::Alternate) {
        s.remoteProgress = std::move(alternateProgress);
        s.remotePosition = alternatePosition;
        LOG_DBG("KOSync", "Selected alternate remote record after mapped-position comparison");
      } else {
        LOG_DBG("KOSync", "Kept primary remote record after mapped-position comparison");
      }
    }
  }

  if (!smart) return Decision::NeedsChoice;
  const ProgressComparison comparison =
      compareProgress(s.localPosition, s.localProgress.percentage, s.remotePosition, s.remoteProgress.percentage);
  LOG_DBG("KOSync", "Smart decision: doc=%s result=%d local=%.6f remote=%.6f remoteXpath=%s mapped=%d/%d",
          s.documentHash.c_str(), static_cast<int>(comparison), s.localProgress.percentage, s.remoteProgress.percentage,
          s.remoteProgress.progress.c_str(), s.remotePosition.spineIndex, s.remotePosition.pageNumber);
  switch (comparison) {
    case ProgressComparison::Synchronized:
      return Decision::Synchronized;
    case ProgressComparison::LocalAhead:
      return Decision::LocalAhead;
    case ProgressComparison::RemoteAhead:
      return Decision::RemoteAhead;
    case ProgressComparison::Unknown:
      break;
  }
  LOG_DBG("KOSync", "Smart sync comparison unknown; manual selection needed");
  return Decision::NeedsChoice;
}

KOReaderSyncClient::Error upload(Session& s) {
  if (s.documentHash.empty()) {
    s.documentHash = calculateDocumentHashForMethod(s.epubPath, KOREADER_STORE.getMatchMethod());
  }
  const KOReaderProgress progress = buildUpload(s);
  // Release the Epub before the network call so the TLS handshake has enough free heap.
  s.epub.reset();
  WifiPowerSaveGuard psGuard;
  return KOReaderSyncClient::updateProgress(progress);
}

bool applyRemote(Session& s) {
  if (!s.epub) return false;
  std::optional<uint32_t> offset;
  if (s.remotePosition.hasVisibleTextOffset) offset = s.remotePosition.visibleTextOffset;
  return EpubReaderUtils::saveProgress(*s.epub, s.remotePosition.spineIndex, s.remotePosition.pageNumber, 0, offset);
}

}  // namespace koreaderSync

namespace sleepSync {
namespace {
// Held only from prepareForSleep to the sleep sync, and only with Sync on Sleep on.
std::optional<koreaderSync::Session> pendingSession;
constexpr unsigned long RESULT_HOLD_MS = 2000;

void popup(GfxRenderer& renderer, const char* message) { GUI.drawPopup(renderer, message); }

void popupStatus(const char* message, void* user) { popup(*static_cast<GfxRenderer*>(user), message); }

void showResult(GfxRenderer& renderer, const char* message) {
  popup(renderer, message);
  delay(RESULT_HOLD_MS);
}

void showFailure(GfxRenderer& renderer, const char* reason) {
  char text[128];
  if (reason && reason[0]) {
    snprintf(text, sizeof(text), "%s: %s", tr(STR_SYNC_FAILED_MSG), reason);
  } else {
    snprintf(text, sizeof(text), "%s", tr(STR_SYNC_FAILED_MSG));
  }
  showResult(renderer, text);
}
}  // namespace

void capture(std::shared_ptr<Epub> epub, const CrossPointPosition& position, const SavedProgressPosition& saved) {
  pendingSession.emplace();
  pendingSession->epubPath = epub->getPath();
  pendingSession->localPosition = position;
  pendingSession->localProgress = saved;
  // Share the reader's open book: it stays alive until goToSleep(), so no second load.
  pendingSession->epub = std::move(epub);
}

bool pending() { return pendingSession.has_value(); }

void run(GfxRenderer& renderer) {
  if (!pendingSession) return;
  koreaderSync::Session& s = *pendingSession;
  using koreaderSync::Decision;
  const Decision decision = koreaderSync::fetchAndCompare(s, renderer, /*smart=*/true, popupStatus, &renderer);
  switch (decision) {
    case Decision::Failed:
      showFailure(renderer, s.error);
      break;
    case Decision::NoRemote:
    case Decision::LocalAhead: {
      popup(renderer, tr(STR_UPLOAD_PROGRESS));
      const auto result = koreaderSync::upload(s);
      if (result == KOReaderSyncClient::OK) {
        showResult(renderer, tr(STR_UPLOAD_SUCCESS));
      } else {
        showFailure(renderer, KOReaderSyncClient::errorString(result));
      }
      break;
    }
    case Decision::RemoteAhead:
      if (koreaderSync::applyRemote(s)) {
        showResult(renderer, tr(STR_SYNC_APPLIED_REMOTE));
      } else {
        showFailure(renderer, tr(STR_SAVE_PROGRESS_FAILED));
      }
      break;
    case Decision::Synchronized:
      showResult(renderer, tr(STR_ALREADY_SYNCED));
      break;
    case Decision::NeedsChoice:
      showResult(renderer, tr(STR_SYNC_NEEDS_CHOICE));
      break;
  }
  pendingSession.reset();
}

void skip(GfxRenderer& renderer, const char* reason) {
  if (!pendingSession) return;
  pendingSession.reset();
  showFailure(renderer, reason);
}

}  // namespace sleepSync
