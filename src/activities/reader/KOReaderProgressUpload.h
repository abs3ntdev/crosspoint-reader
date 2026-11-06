#pragma once
#include <Epub.h>

#include <string>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

namespace koreaderUpload {
// Builds the progress record for a push to the configured sync server: rich
// position for servers that support it, and metadata plus the plugin sidecar
// when Send Metadata is on. epub may be null; only the filename is sent then.
KOReaderProgress build(const std::string& epubPath, const std::string& documentHash, const CrossPointPosition& position,
                       const SavedProgressPosition& saved, Epub* epub);

// Sync on sleep. capture() runs from the reader's prepareForSleep while the
// book is still open; push() runs from the sleep path once WiFi is up.
void capture(const std::string& epubPath, const CrossPointPosition& position, const SavedProgressPosition& saved,
             Epub* epub);
bool pending();
// Clipping sync (when enabled) then progress upload. Clears the capture.
// The caller must hold no render lock.
void push(GfxRenderer& renderer);
}  // namespace koreaderUpload
