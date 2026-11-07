/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_ui_ios.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "xenia/base/autorelease_pool_mac.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/audio_helper.h"
#include "xenia/ui/ios/app/windowed_app_context_ios.h"
#include "xenia/ui/window.h"

DECLARE_bool(achievement_notification_position_by_game);

namespace xe::kernel::xam {

// Reuse Edge's guest notification ordering and overlapped completion helpers.
X_RESULT xeXamDispatchWithoutDialog(std::function<X_RESULT()> run,
                                    uint32_t overlapped);

namespace ios {
namespace {

// Like xeXamDispatchWithoutDialog, for a result with an extended error and a
// length.
X_RESULT DispatchWithoutDialogEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> run, uint32_t overlapped) {
  auto pre = []() {
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto post = []() {
    xe::threading::Sleep(std::chrono::milliseconds(200));
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);
    post();
    return result;
  }
  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
  return X_ERROR_IO_PENDING;
}

ui::IOSWindowedAppContext* GetContext(KernelState* state) {
  auto* emulator = state ? state->emulator() : nullptr;
  auto* window = emulator ? emulator->display_window() : nullptr;
  return window ? dynamic_cast<ui::IOSWindowedAppContext*>(&window->app_context())
                : nullptr;
}

// Native prompts marshal presentation to UIKit and wait for the user's answer.
// A cooperative guest must yield its dispatcher rather than block that CPU.
X_RESULT RunPrompt(const std::function<X_RESULT()>& run) {
  if (!XThread::GetCurrentFiberThread()) {
    ScopedAutoreleasePool pool("iOS guest prompt");
    return run();
  }
  X_RESULT result = X_ERROR_FUNCTION_FAILED;
  threading::Fence completed;
  std::thread worker([&]() {
    ScopedAutoreleasePool pool("iOS guest prompt worker");
    try {
      result = run();
    } catch (const std::exception& error) {
      XELOGE("iOS guest UI failed: {}", error.what());
    } catch (...) {
      XELOGE("iOS guest UI failed with an unknown exception");
    }
    completed.Signal();
  });
  GuestScheduler::WaitOnFence(completed);
  worker.join();
  return result;
}

bool BeginDialog(KernelState* state, ui::IOSWindowedAppContext* context,
                 bool already_active) {
  if (!context || context->IsInUIThread()) {
    if (state && already_active) {
      state->xam_state()->is_xam_dialog_present_.store(false);
    }
    XELOGW("iOS guest prompts require an available UI and a non-UI caller");
    return false;
  }
  if (!already_active) {
    bool expected = false;
    return state->xam_state()->is_xam_dialog_present_.compare_exchange_strong(
        expected, true);
  }
  return true;
}

struct DialogCompletion {
  KernelState* state;
  ~DialogCompletion() {
    state->xam_state()->is_xam_dialog_present_.store(false);
  }
};

X_RESULT DispatchDialog(std::function<X_RESULT(ui::IOSWindowedAppContext&)>&& run,
                        uint32_t overlapped, bool already_active = false) {
  auto* state = kernel_state();
  auto* context = GetContext(state);
  if (!BeginDialog(state, context, already_active)) {
    return context ? X_ERROR_ACCESS_DENIED : X_ERROR_FUNCTION_FAILED;
  }
  XELOGI("iOS XAM: native dialog requested (overlapped={:08X})", overlapped);
  return xeXamDispatchWithoutDialog(
      [state, context, run = std::move(run)]() {
        DialogCompletion completion{state};
        const X_RESULT result = RunPrompt([&]() { return run(*context); });
        XELOGI("iOS XAM: native dialog completed with {:08X}", result);
        return result;
      },
      overlapped);
}

}  // namespace

X_RESULT ShowMessageBox(std::string title, std::string text,
                       std::vector<std::string> buttons,
                       uint32_t default_button,
                       std::function<X_RESULT(uint32_t)> selected,
                       uint32_t overlapped, bool already_active) {
  if (buttons.empty()) {
    buttons.emplace_back("OK");
  }
  default_button = std::min(default_button, uint32_t(buttons.size() - 1));
  return DispatchDialog(
      [title = std::move(title), text = std::move(text),
       buttons = std::move(buttons), default_button,
       selected = std::move(selected)](ui::IOSWindowedAppContext& context) {
        uint32_t choice = default_button;
        if (!context.PromptMessageBoxUI(title, text, buttons, default_button,
                                        &choice)) {
          return X_ERROR_CANCELLED;
        }
        return selected ? selected(choice) : X_ERROR_SUCCESS;
      },
      overlapped, already_active);
}

int ChooseOption(std::string title, std::string text,
                 std::vector<std::string> options) {
  auto* context = GetContext(kernel_state());
  if (!context || context->IsInUIThread() || options.empty()) {
    XELOGW("iOS prompt: no UI to ask \"{}\" on", title);
    return -1;
  }
  uint32_t choice = 0;
  bool chosen = false;
  RunPrompt([&]() {
    chosen = context->PromptMessageBoxUI(title, text, options, 0, &choice);
    return X_ERROR_SUCCESS;
  });
  return chosen && choice < options.size() ? int(choice) : -1;
}

X_RESULT ShowKeyboard(uint32_t user_index, std::string title,
                      std::string description, std::string default_text,
                      size_t max_length,
                      std::function<void(const std::u16string&)> write_text,
                      uint32_t overlapped) {
  auto* state = kernel_state();
  auto* context = GetContext(state);
  if (!BeginDialog(state, context, false)) {
    return context ? X_ERROR_ACCESS_DENIED : X_ERROR_FUNCTION_FAILED;
  }
  XELOGI("iOS XAM: native keyboard requested (overlapped={:08X})", overlapped);
  return DispatchWithoutDialogEx(
      [state, context, title = std::move(title),
       description = std::move(description),
       default_text = std::move(default_text), max_length,
       write_text = std::move(write_text)](
          uint32_t& extended_error, uint32_t& length) {
        DialogCompletion completion{state};
        extended_error = X_ERROR_CANCELLED;
        length = 0;
        return RunPrompt([&]() -> X_RESULT {
          std::string text;
          bool cancelled = true;
          if (!context->PromptKeyboardUI(title, description, default_text,
                                          max_length, &text,
                                          &cancelled)) {
            return X_ERROR_FUNCTION_FAILED;
          }
          if (!cancelled) {
            write_text(xe::to_utf16(text));
            extended_error = X_ERROR_SUCCESS;
          }
          // Match Edge's keyboard completion: cancellation is the extended
          // result; the completed UI request itself succeeds.
          return X_ERROR_SUCCESS;
        });
      },
      overlapped);
}

X_RESULT ShowSignIn(uint32_t user_index, uint32_t users_needed) {
  return DispatchDialog(
      [=](ui::IOSWindowedAppContext& context) -> X_RESULT {
        return context.PromptSignInUI(user_index, users_needed)
                   ? X_ERROR_SUCCESS
                   : X_ERROR_CANCELLED;
      },
      0);
}

X_RESULT ShowAchievements(uint32_t user_index, uint32_t title_id) {
  return DispatchDialog(
      [=](ui::IOSWindowedAppContext& context) -> X_RESULT {
        return context.PromptAchievementsUI(user_index, title_id)
                   ? X_ERROR_SUCCESS
                   : X_ERROR_CANCELLED;
      },
      0);
}

X_RESULT ShowGamerCard(uint32_t user_index, uint64_t xuid) {
  XELOGI("iOS XAM: gamer card for user {} ({:016X})", user_index, xuid);
  return DispatchDialog(
      [=](ui::IOSWindowedAppContext& context) -> X_RESULT {
        return context.PromptGamerCardUI(user_index, xuid) ? X_ERROR_SUCCESS
                                                           : X_ERROR_CANCELLED;
      },
      0);
}

void Notify(std::string text, uint32_t user_index, uint8_t position) {
  auto* context = GetContext(kernel_state());
  if (!context) {
    return;
  }
  ui::AchievementNotificationPayload payload;
  payload.user_index = uint8_t(user_index);
  payload.position_id = position;
  payload.title = "Notification";
  payload.description = std::move(text);
  context->PresentAchievementNotification(payload);
}

void NotifyAchievement(KernelState* state, uint64_t xuid, uint32_t title_id,
                       const Achievement& achievement) {
  auto* context = GetContext(state);
  if (!context) {
    return;
  }
  ui::AchievementNotificationPayload payload;
  payload.xuid = xuid;
  payload.user_index =
      state->xam_state()->profile_manager()->GetUserIndexAssignedToProfile(xuid);
  payload.title_id = title_id;
  payload.achievement_id = achievement.achievement_id;
  payload.gamerscore = achievement.gamerscore;
  payload.title = "Achievement unlocked";
  payload.description = fmt::format("{}G - {}", achievement.gamerscore,
                                    xe::to_utf8(achievement.achievement_name));
  payload.position_id = cvars::achievement_notification_position_by_game
                            ? state->notification_position_
                            : 2;
  auto icon = state->xam_state()->achievement_manager()->GetAchievementIcon(
      xuid, title_id, achievement.achievement_id);
  if (icon.size() <= 512 * 1024) {
    payload.icon_data.assign(icon.begin(), icon.end());
  }
  context->CallInUIThread([context, payload = std::move(payload)]() {
    ui::AudioHelper::Instance().PlayAchievementSound();
    context->PresentAchievementNotification(payload);
  });
}

}  // namespace ios
}  // namespace xe::kernel::xam
