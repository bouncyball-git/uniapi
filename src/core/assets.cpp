// Embedded files' bytes (include/uniapi/assets.hpp). Compressed ones (uniapi_embed()'s COMPRESS,
// tools/embed): assetData() inflates one on first use and keeps it, for code that reads a file
// itself; serving a page inflates one for the request alone and keeps nothing (InflateAsset).

#include <uniapi/assets.hpp>

#include "core/asset_bytes.hpp"

#include "miniz.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace uniapi {

namespace detail {

std::shared_ptr<const std::vector<unsigned char>> InflateAsset(const Asset& asset) {
    auto out = std::make_shared<std::vector<unsigned char>>(asset.size + 1);  // +1: never zero-sized
    mz_ulong size = static_cast<mz_ulong>(asset.size);
    if (mz_uncompress(out->data(), &size, asset.data, static_cast<mz_ulong>(asset.packedSize)) != MZ_OK || size != asset.size) {
        return nullptr;
    }
    out->resize(asset.size);
    return out;
}

}  // namespace detail

AssetData assetData(const Asset& asset) {
    if (asset.packedSize == 0) return {asset.data, asset.size};
    static std::mutex mutex;
    static std::unordered_map<const Asset*, std::shared_ptr<const std::vector<unsigned char>>> inflated;
    std::lock_guard<std::mutex> lock(mutex);
    auto& bytes = inflated[&asset];
    if (!bytes) {
        bytes = detail::InflateAsset(asset);
        if (!bytes) {
            inflated.erase(&asset);
            return {nullptr, 0};
        }
    }
    return {bytes->data(), asset.size};
}

}  // namespace uniapi
