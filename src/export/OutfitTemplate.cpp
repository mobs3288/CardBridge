// cardbridge - the embedded outfit-icon background template.

#include "export/OutfitTemplate.h"

#include "card/MaterialEdits.h"
#include "core/Bytes.h"

#include "export/outfit_template.inc"

namespace cardbridge::exporter {

bool DecodeOutfitTemplate(std::vector<uint8_t>& rgba, int& width, int& height) {
    Bytes bytes;
    bytes.assign(kOutfitTemplateJpeg, kOutfitTemplateJpeg + kOutfitTemplateJpegSize);
    return card::DecodeImageBytes(bytes, width, height, rgba);
}

} // namespace cardbridge::exporter
