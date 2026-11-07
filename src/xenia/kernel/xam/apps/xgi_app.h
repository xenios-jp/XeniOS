/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_APPS_XGI_APP_H_
#define XENIA_KERNEL_XAM_APPS_XGI_APP_H_

#include <string_view>
#include <vector>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xam/xam_live_sessions.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

class XgiApp : public App {
 public:
  explicit XgiApp(KernelState* kernel_state);

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  X_HRESULT ReadUserPreference(uint32_t user_index, uint32_t preference,
                               uint32_t& value);
  X_HRESULT WriteUserPreference(uint32_t user_index, uint32_t preference,
                                uint32_t value);
  // XSessionSearch and XSessionSearchEx over the sessions announced on the
  // title's lobby.
  struct SearchQuery {
    uint32_t proc_index = 0;
    uint32_t max_results = 0;
    uint16_t num_props = 0;
    uint16_t num_contexts = 0;
    uint32_t props_ptr = 0;
    uint32_t contexts_ptr = 0;
    uint32_t buffer_size = 0;
    uint32_t results_ptr = 0;
  };
  X_HRESULT SessionSearch(const uint8_t* args);
  X_HRESULT SessionSearchWeighted(const uint8_t* args);
  X_HRESULT SessionSearch(const SearchQuery& query, std::string_view name);
  X_HRESULT SessionSearchByIds(uint8_t* args, const uint8_t* ids,
                               uint32_t id_count, uint32_t size_offset,
                               uint32_t results_offset);
  uint32_t WriteSearchResults(const std::vector<LiveSessionAdvert>& sessions,
                              uint32_t max_results, uint32_t buffer_size,
                              uint32_t results_ptr);
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_APPS_XGI_APP_H_
