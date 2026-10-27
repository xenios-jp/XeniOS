/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_UPLOAD_CACHE_H_
#define XENIA_GPU_METAL_METAL_UPLOAD_CACHE_H_

namespace xe {
namespace gpu {
namespace metal {

// Remembers the last upload of immutable data. Key equality must imply
// identical uploaded bytes - it is an exact key, not a hash or a pointer
// identity. Reset before the memory holding the slice can be reused.
template <typename Slice, typename Key>
class UploadCache {
 public:
  void Reset() { valid_ = false; }

  // Upload is bool(Slice& out). Returns the previous slice when the key is
  // unchanged, otherwise uploads a new one. A failed upload keeps the previous
  // entry.
  template <typename Upload>
  bool GetOrUpload(const Key& key, Upload&& upload, Slice& out) {
    if (valid_ && key == key_) {
      out = slice_;
      return true;
    }
    Slice fresh{};
    if (!upload(fresh)) {
      return false;
    }
    key_ = key;
    slice_ = fresh;
    valid_ = true;
    out = fresh;
    return true;
  }

 private:
  bool valid_ = false;
  Key key_{};
  Slice slice_{};
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_UPLOAD_CACHE_H_
