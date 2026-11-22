// Online co-op compat (../README.md): the user folder (the app's internal files: <files>/online; the identity key,
// the block list and the settings live there, never in the shared saves folder, so a synced saves folder doesn't
// copy this install's identity to another device).
#pragma once
#include <string>
namespace host {
std::string config_dir();
}
