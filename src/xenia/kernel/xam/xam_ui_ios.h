/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_UI_IOS_H_
#define XENIA_KERNEL_XAM_XAM_UI_IOS_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "xenia/xbox.h"

namespace xe::kernel {
class KernelState;
namespace xam {
struct Achievement;
namespace ios {

// The caller retains argument validation and guest-memory result conversion.
// These functions own native presentation and the SystemUI/completion lifetime.
X_RESULT ShowMessageBox(std::string title, std::string text,
                       std::vector<std::string> buttons,
                       uint32_t default_button,
                       std::function<X_RESULT(uint32_t)> selected,
                       uint32_t overlapped = 0, bool already_active = false);
// The text is at most max_length UTF-16 units.
X_RESULT ShowKeyboard(uint32_t user_index, std::string title,
                      std::string description, std::string default_text,
                      size_t max_length,
                      std::function<void(const std::u16string&)> write_text,
                      uint32_t overlapped);
X_RESULT ShowSignIn(uint32_t user_index, uint32_t users_needed);
// For the emulator's own questions to the user, such as a disc swap: the index
// of the option chosen, or -1 if the prompt was cancelled or could not show.
int ChooseOption(std::string title, std::string text,
                 std::vector<std::string> options);
X_RESULT ShowAchievements(uint32_t user_index, uint32_t title_id);
void Notify(std::string text, uint32_t user_index, uint8_t position);
void NotifyAchievement(KernelState* state, uint64_t xuid, uint32_t title_id,
                       const Achievement& achievement);

}  // namespace ios
}  // namespace xam
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_XAM_XAM_UI_IOS_H_
