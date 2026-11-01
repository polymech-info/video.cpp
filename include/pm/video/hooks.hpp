#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace pm::video {

void set_exe_dir(std::function<std::filesystem::path()> fn);
void set_exe_path(std::function<std::filesystem::path()> fn);
void set_cli_path(std::function<std::filesystem::path()> fn);
void set_models_dir(std::function<std::filesystem::path()> fn);
void set_embedding_models_dir(std::function<std::filesystem::path()> fn);
void set_log(std::function<void(int level, const std::string& message)> fn);
void set_key_label(std::function<std::string(unsigned mods, unsigned vk, bool modifiers)> fn);
void set_config_dir(std::function<std::filesystem::path()> fn);
void set_parse_shortcut(bool (*fn)(const std::string&, unsigned&, unsigned&));
void set_ui_log_line(std::function<void(const std::string&)> fn);
void set_caption_status(std::function<std::shared_ptr<void>(const std::string& mode, const std::string& label)> fn);
void set_record_in_cli_process(bool on);

std::filesystem::path exe_dir();
std::filesystem::path exe_path();
std::filesystem::path cli_path();
std::filesystem::path models_dir();
std::filesystem::path embedding_models_dir();
std::filesystem::path config_dir();

void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);
std::string key_label(unsigned mods, unsigned vk, bool modifiers);
bool parse_shortcut(const std::string& text, unsigned& mods, unsigned& vk);
void ui_log_line(const std::string& message);
std::shared_ptr<void> caption_status(const std::string& mode, const std::string& label);
bool record_in_cli_process();

} // namespace pm::video
