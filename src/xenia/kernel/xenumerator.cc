/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xenumerator.h"

#include <cstring>

#include "xenia/base/assert.h"

#include "xenia/kernel/kernel_state.h"
#include "xenia/vfs/devices/stfs_xbox.h"

namespace xe {
namespace kernel {

XEnumerator::XEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                         size_t item_size)
    : XObject(kernel_state, kObjectType),
      items_per_enumerate_(items_per_enumerate),
      item_size_(item_size) {}

XEnumerator::~XEnumerator() = default;

X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,
                                 uint32_t open_message, uint32_t close_message,
                                 uint32_t flags, uint32_t extra_size,
                                 void** extra_buffer) {
  auto native_object = CreateNative(sizeof(X_KENUMERATOR) + extra_size);
  if (!native_object) {
    return X_STATUS_NO_MEMORY;
  }
  auto guest_object = reinterpret_cast<X_KENUMERATOR*>(native_object);
  guest_object->app_id = app_id;
  guest_object->open_message = open_message;
  guest_object->close_message = close_message;
  guest_object->user_index = user_index;
  guest_object->items_per_enumerate =
      static_cast<uint32_t>(items_per_enumerate_);
  guest_object->flags = flags;
  if (extra_buffer) {
    *extra_buffer =
        !extra_buffer ? nullptr : &native_object[sizeof(X_KENUMERATOR)];
  }

  extra_size_ = extra_size;
  return X_STATUS_SUCCESS;
}

X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,
                                 uint32_t open_message, uint32_t close_message,
                                 uint32_t flags) {
  return Initialize(user_index, app_id, open_message, close_message, flags, 0,
                    nullptr);
}

uint8_t* XStaticUntypedEnumerator::AppendItem() {
  size_t offset = buffer_.size();
  buffer_.resize(offset + item_size());
  item_count_++;
  return const_cast<uint8_t*>(&buffer_.data()[offset]);
}

uint32_t XStaticUntypedEnumerator::WriteItems(uint8_t* buffer_data,
                                              uint32_t buffer_size,
                                              uint32_t* written_count) {
  const size_t actual_count =
      std::min(item_count_ - current_item_, items_per_enumerate());
  if (!actual_count) {
    return X_ERROR_NO_MORE_FILES;
  }

  assert_false(buffer_size < item_size());

  const size_t available_count = buffer_size / item_size();
  const size_t size = actual_count * item_size();
  const size_t offset = current_item_ * item_size();

  // Some enumerators expect buffer_size to be respected for now assert.
  assert_false(actual_count > available_count);

  std::memcpy(buffer_data, buffer_.data() + offset, size);

  current_item_ += actual_count;

  if (written_count) {
    *written_count = static_cast<uint32_t>(actual_count);
  }

  return X_ERROR_SUCCESS;
}

uint32_t XAchievementEnumerator::WriteItems(uint8_t* buffer_data,
                                            uint32_t buffer_size,
                                            uint32_t* written_count) {
  const size_t actual_count =
      std::min(items_.size() - current_item_, items_per_enumerate());
  if (!actual_count) {
    return X_ERROR_NO_MORE_FILES;
  }

  const size_t available_count = buffer_size / item_size();

  // Unknown if buffer_size should be respected.
  assert_false(actual_count > available_count);

  auto details = reinterpret_cast<xam::X_ACHIEVEMENT_DETAILS*>(buffer_data);
  size_t string_offset =
      items_per_enumerate() * sizeof(xam::X_ACHIEVEMENT_DETAILS);
  auto string_buffer = StringBuffer{
      &buffer_data[string_offset],
      actual_count * xam::X_ACHIEVEMENT_DETAILS::kStringBufferSize};
  for (size_t i = 0; i < actual_count; ++i, ++current_item_) {
    const auto& item = items_[current_item_];
    details[i].id = item.id;
    details[i].label_ptr =
        !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
    details[i].description_ptr =
        !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
    details[i].unachieved_ptr =
        !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
    details[i].image_id = item.image_id;
    details[i].gamerscore = item.gamerscore;
    details[i].unlock_time.high_part = item.unlock_time.high_part;
    details[i].unlock_time.low_part = item.unlock_time.low_part;
    details[i].flags = item.flags;
  }

  if (written_count) {
    *written_count = static_cast<uint32_t>(actual_count);
  }

  return X_ERROR_SUCCESS;
}

uint32_t XTitleEnumerator::WriteItems(uint8_t* buffer_data,
                                      uint32_t buffer_size,
                                      uint32_t* written_count) {
  const size_t actual_count =
      std::min(items_.size() - current_item_, items_per_enumerate());
  if (!actual_count) {
    return X_ERROR_NO_MORE_FILES;
  }

  const size_t available_count = buffer_size / item_size();

  // Unknown if buffer_size should be respected.
  assert_false(actual_count > available_count);

  auto details = reinterpret_cast<XTITLE_PLAYED*>(buffer_data);

  for (size_t i = 0; i < actual_count; ++i, ++current_item_) {
    const auto& item = items_[current_item_];
    details[i].base.title_id = item.id;
    details[i].base.achievements_count = item.achievements_count;
    details[i].base.achievements_unlocked = item.unlocked_achievements_count;
    details[i].base.gamerscore_total = item.gamerscore_amount;
    details[i].base.gamerscore_earned = item.title_earned_gamerscore;
    details[i].base.online_achievement_count =
        item.online_unlocked_achievements;
    details[i].base.all_avatar_awards.earned = item.all_avatar_awards.earned;
    details[i].base.all_avatar_awards.possible =
        item.all_avatar_awards.possible;
    details[i].base.male_avatar_awards.earned = item.male_avatar_awards.earned;
    details[i].base.male_avatar_awards.possible =
        item.male_avatar_awards.possible;
    details[i].base.female_avatar_awards.earned =
        item.female_avatar_awards.earned;
    details[i].base.female_avatar_awards.possible =
        item.female_avatar_awards.possible;
    details[i].base.flags = item.flags;

    // On console if title is played offline this field is set to 0.
    details[i].base.last_played = X_FILETIME((uint64_t)0);
    string_util::copy_and_swap_truncating(
        reinterpret_cast<char16_t*>(details[i].title_name), item.title_name,
        xe::countof(details[i].title_name));
  }

  if (written_count) {
    *written_count = static_cast<uint32_t>(actual_count);
  }

  return X_ERROR_SUCCESS;
}

void XUserStatsEnumerator::WriteEmptyResults(
    uint8_t* buffer, uint32_t buffer_size, uint32_t buffer_guest,
    const std::vector<uint32_t>& view_ids) {
  assert_true(buffer_size >= EmptyResultsSize(view_ids.size()));
  std::memset(buffer, 0, buffer_size);
  auto results = reinterpret_cast<XUSER_STATS_READ_RESULTS*>(buffer);
  results->num_views = static_cast<uint32_t>(view_ids.size());
  results->views_ptr = buffer_guest + sizeof(XUSER_STATS_READ_RESULTS);
  const uint32_t rows_ptr = buffer_guest + EmptyResultsSize(view_ids.size());
  auto views = reinterpret_cast<XUSER_STATS_VIEW*>(results + 1);
  for (size_t i = 0; i < view_ids.size(); ++i) {
    views[i].view_id = view_ids[i];
    views[i].rows_ptr = rows_ptr;
  }
}

uint32_t XUserStatsEnumerator::WriteItems(uint8_t* buffer_data,
                                          uint32_t buffer_size,
                                          uint32_t* written_count) {
  if (written_) {
    return X_ERROR_NO_MORE_FILES;
  }

  if (buffer_size < buffer_size_) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  // There is no stats server so every view reads back with no rows.
  WriteEmptyResults(buffer_data, buffer_size,
                    memory()->HostToGuestVirtual(buffer_data), view_ids_);
  written_ = true;

  if (written_count) {
    *written_count = 1;
  }

  return X_ERROR_SUCCESS;
}

uint32_t XMPCreateUserPlaylistEnumerator::WriteItems(uint8_t* buffer_data,
                                                     uint32_t buffer_size,
                                                     uint32_t* written_count) {
  // Fixed 545408C0 freezing at main menu.
  std::memset(buffer_data, 0, buffer_size);

  const size_t actual_count =
      std::min(items_.size() - current_item_, items_per_enumerate());
  if (!actual_count) {
    return X_ERROR_NO_MORE_FILES;
  }

  const size_t available_count = buffer_size / item_size();
  const size_t valid_count = std::min(actual_count, available_count);

  xam::XMP_USER_PLAYLIST_INFO* results =
      reinterpret_cast<xam::XMP_USER_PLAYLIST_INFO*>(buffer_data);

  std::copy_n(items_.begin() + current_item_, valid_count, results);

  current_item_ += valid_count;

  if (written_count) {
    *written_count = static_cast<uint32_t>(valid_count);
  }

  return X_ERROR_SUCCESS;
}

uint32_t ProfileEnumerator::WriteItems(uint8_t* buffer_data,
                                       uint32_t buffer_size,
                                       uint32_t* written_count) {
  const size_t actual_count =
      std::min(items_.size() - current_item_, items_per_enumerate());
  if (!actual_count) {
    return X_ERROR_NO_MORE_FILES;
  }

  // FFFE07D1 provides buffer_size of 0.
  xam::X_PROFILEENUMRESULT* profiles =
      reinterpret_cast<xam::X_PROFILEENUMRESULT*>(buffer_data);

  std::copy_n(items_.begin() + current_item_, actual_count, profiles);

  current_item_ += actual_count;

  if (written_count) {
    *written_count = static_cast<uint32_t>(actual_count);
  }

  return X_ERROR_SUCCESS;
}

uint32_t ContentEnumerator::WriteItems(uint8_t* buffer_data,
                                       uint32_t buffer_size,
                                       uint32_t* written_count) {
  const size_t actual_count =
      std::min(items_.size() - current_item_, items_per_enumerate());
  if (!actual_count) {
    return X_ERROR_NO_MORE_FILES;
  }

  // 4E4D07F0 does not respect buffer_size, possibly bug?
  xam::XCONTENT_DATA* contents =
      reinterpret_cast<xam::XCONTENT_DATA*>(buffer_data);

  std::copy_n(items_.begin() + current_item_, actual_count, contents);

  current_item_ += actual_count;

  if (written_count) {
    *written_count = static_cast<uint32_t>(actual_count);
  }

  return X_ERROR_SUCCESS;
}

// Reads the package's header, so only for items being written.
static xam::XCONTENT_DATA_INTERNAL InternalContentData(
    KernelState* kernel_state, const xam::XCONTENT_AGGREGATE_DATA& data) {
  xam::XCONTENT_DATA_INTERNAL item = {};
  item.device_id = data.device_id;
  item.content_type = data.content_type;
  item.display_name_raw = data.display_name_raw;
  std::memcpy(item.file_name_raw, data.file_name_raw,
              sizeof(data.file_name_raw));
  item.xuid = data.xuid;
  item.title_id = data.title_id;

  std::vector<uint8_t> metadata;
  if (!kernel_state->content_manager()->GetContentMetadata(data.xuid, data,
                                                           &metadata)) {
    const auto* header =
        reinterpret_cast<const vfs::XContentMetadata*>(metadata.data());
    item.content_size = header->content_size;
    xe::string_util::copy_and_swap_truncating(
        item.title_name, header->title_name(), xe::countof(item.title_name));
  }
  return item;
}

uint32_t ContentAggregateEnumerator::WriteItems(uint8_t* buffer_data,
                                                uint32_t buffer_size,
                                                uint32_t* written_count) {
  return WriteItems(buffer_data, buffer_size, written_count, 0);
}

uint32_t ContentAggregateEnumerator::WriteItems(uint8_t* buffer_data,
                                                uint32_t buffer_size,
                                                uint32_t* written_count,
                                                uint32_t flags) {
  assert_false(buffer_size < item_size());
  const size_t available_count =
      std::min(items_per_enumerate(), buffer_size / item_size());
  size_t count = 0;
  for (; current_item_ < items_.size() && count < available_count;
       ++current_item_) {
    const Item& item = items_[current_item_];
    if (item.other_title && !(flags & kAllTitlesFlag)) {
      continue;
    }
    const xam::XCONTENT_DATA_INTERNAL data =
        InternalContentData(kernel_state(), item.data);
    std::memcpy(buffer_data + count * item_size(), &data, item_size());
    ++count;
  }
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  if (written_count) {
    *written_count = static_cast<uint32_t>(count);
  }

  return X_ERROR_SUCCESS;
}

}  // namespace kernel
}  // namespace xe
