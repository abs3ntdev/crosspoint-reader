#include "KOReaderProgressUpload.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <utility>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "activities/RenderLock.h"
#include "clippings/ClippingSync.h"
#include "network/WifiPowerSaveGuard.h"

namespace koreaderUpload {

KOReaderProgress build(const std::string& epubPath, const std::string& documentHash, const CrossPointPosition& position,
                       const SavedProgressPosition& saved, Epub* epub) {
  KOReaderProgress progress{};
  progress.document = documentHash;
  progress.progress = saved.xpath;
  progress.percentage = saved.percentage;

  // The HTTP client enforces the same server-type boundary before serializing.
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition pos;
    const float pct = saved.percentage < 0.0f ? 0.0f : saved.percentage > 1.0f ? 1.0f : saved.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(position.spineIndex);
    pos.pageNumber = static_cast<uint16_t>(position.pageNumber);
    pos.totalPages = static_cast<uint16_t>(position.totalPages > 0 ? position.totalPages : 1);
    if (position.hasParagraphIndex) pos.paragraphIndex = position.paragraphIndex;
    pos.xpath = saved.xpath;
    progress.position = std::move(pos);
  }

  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
      if (KOREADER_STORE.supportsExtendedMetadata()) {
        Epub::SyncMetadata syncMetadata;
        if (epub->loadSyncMetadata(syncMetadata)) {
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
    Storage.readFileToString("KOSync", epubPath + ".meta.json", 2 * 1024, meta.extraJson);
    progress.metadata = std::move(meta);
  }
  return progress;
}

namespace {
// Held only between prepareForSleep and the sleep push, and only when Sync on
// Sleep is on: a few small strings, released by push().
std::string pendingPath;
KOReaderProgress pendingProgress{};
bool hasPending = false;
}  // namespace

void capture(const std::string& epubPath, const CrossPointPosition& position, const SavedProgressPosition& saved,
             Epub* epub) {
  const std::string hash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                               ? KOReaderDocumentId::calculateFromFilename(epubPath)
                               : KOReaderDocumentId::calculate(epubPath);
  if (hash.empty()) {
    LOG_ERR("KOSync", "Sleep sync: document hash failed");
    return;
  }
  pendingPath = epubPath;
  pendingProgress = build(epubPath, hash, position, saved, epub);
  hasPending = true;
}

bool pending() { return hasPending; }

void push(GfxRenderer& renderer) {
  if (!hasPending) return;
  hasPending = false;
  if (WiFi.status() == WL_CONNECTED) {
    WifiPowerSaveGuard psGuard;
    bool clippingsOk = true;
    if (KOREADER_STORE.getSyncClippings()) {
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
      RenderLock lock;
      GfxRenderer::FrameBufferLoan loan(renderer);
#endif
      clippingsOk = clippingSync::run(pendingPath, pendingProgress.document);
    }
    (void)renderer;
    if (!clippingsOk) LOG_ERR("KOSync", "Sleep sync: clipping sync failed");
    const auto result = KOReaderSyncClient::updateProgress(pendingProgress);
    LOG_DBG("KOSync", "Sleep sync upload: %s (HTTP %d)", KOReaderSyncClient::errorString(result),
            KOReaderSyncClient::lastHttpCode);
  }
  std::string().swap(pendingPath);
  pendingProgress = KOReaderProgress{};
}

}  // namespace koreaderUpload
