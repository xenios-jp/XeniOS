/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CONFIG_H_
#define XENIA_CONFIG_H_

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include "third_party/tomlplusplus/toml.hpp"

namespace cvar {
class IConfigVar;
}  // namespace cvar

namespace xe {
class Emulator;
}  // namespace xe

toml::parse_result ParseFile(const std::filesystem::path& filename);

namespace config {
extern std::filesystem::path config_folder;
extern std::string game_config_suffix;

// Reads the global config (or the file given with --config). A file that
// doesn't parse never ends the launch: a good global config is copied to
// <config>.bak; a broken one is kept as <config>.invalid-<yyyymmdd-HHMMSS>, the
// backup's values (else the defaults) are loaded, and nothing is written until
// the user chooses (on iOS, via GetConfigLoadProblem; elsewhere the choice is
// made here and the user told). A broken --config file is ignored and left
// alone.
void SetupConfig(const std::filesystem::path& config_folder);

// What a UI needs to ask the user about a global config that couldn't be read.
struct ConfigLoadProblem {
  std::filesystem::path file;       // The config that didn't parse.
  std::filesystem::path kept_copy;  // Where it was kept; empty if that failed.
  std::string error;
  uint32_t line = 0;  // 0 when the error has no position.
  uint32_t column = 0;
  bool backup_available = false;  // A backup parsed and is loaded now.
};
// Non-null while the choice is pending. Main thread.
const ConfigLoadProblem* GetConfigLoadProblem();
// Each ends the pending choice, writes a valid config and returns true; false
// when nothing was pending (or, for the backup, it can't be read now).
bool RestoreConfigBackup();
bool ResetConfigToDefaults();
toml::table LoadGameConfig(uint32_t title_id);
std::filesystem::path GetBundledDataPath(const std::string& subdirectory);

// Extract title_id from a game file and load its config overrides.
// Returns the title_id if successful, 0 otherwise.
uint32_t LoadGameConfigForFile(const std::filesystem::path& game_path);
void SaveConfig();
// Writes the current values of the cvars named in |names| and keeps every other
// key as the config file has it, so an edit made to the file while the app runs
// survives. A key the file lacks, or has with the wrong type, gets its current
// value; a missing or unreadable file gets every current value.
void SaveConfigValues(const std::set<std::string>& names);
void SaveGameConfig(uint32_t title_id, const toml::table& config_table);
// The node holding |var|'s value in a parsed config: at its category, in a
// table stored under the literal dotted category, or in the [GPU] table it
// moved out of. Null if the config has none.
const toml::node* FindConfigNode(const toml::table& root,
                                 const cvar::IConfigVar& var);
// Resolves a possibly dotted category ("GPU.Debug") to a nested table.
toml::table* ResolveSectionTable(toml::table& root, std::string_view section);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, const std::string& value);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, bool value);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, int32_t value);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, uint32_t value);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, double value);
void ReloadConfig();
void SetConfigSavedCallback(std::function<void()> callback);
}  // namespace config

#endif  // XENIA_CONFIG_H_
