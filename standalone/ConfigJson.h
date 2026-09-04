#pragma once

#include "Config.h"
#include "json.hpp"

namespace nam_ui
{

/// \brief The bits of the config's JSON that more than one file writes.
///
/// A metronome setting is stored by the config, by every project, and read back by both. Written
/// once here so a new field is added in one place rather than remembered in four.
namespace config_json
{

AppConfig::MetronomePreset ReadMetronome(const nlohmann::json& j);
nlohmann::json WriteMetronome(const AppConfig::MetronomePreset& preset);

} // namespace config_json
} // namespace nam_ui
