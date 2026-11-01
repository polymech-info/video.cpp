#include "pm/video/hooks.hpp"

#include <iostream>
#include <memory>
#include <mutex>

namespace pm::video {
namespace {

std::mutex& hook_mu()
{
    static std::mutex mu;
    return mu;
}

std::function<std::filesystem::path()>& slot_exe_dir()
{
    static std::function<std::filesystem::path()> fn;
    return fn;
}
std::function<std::filesystem::path()>& slot_exe_path()
{
    static std::function<std::filesystem::path()> fn;
    return fn;
}
std::function<std::filesystem::path()>& slot_cli_path()
{
    static std::function<std::filesystem::path()> fn;
    return fn;
}
std::function<std::filesystem::path()>& slot_models()
{
    static std::function<std::filesystem::path()> fn;
    return fn;
}
std::function<std::filesystem::path()>& slot_embed()
{
    static std::function<std::filesystem::path()> fn;
    return fn;
}
std::function<void(int, const std::string&)>& slot_log()
{
    static std::function<void(int, const std::string&)> fn;
    return fn;
}
std::function<std::string(unsigned, unsigned, bool)>& slot_key()
{
    static std::function<std::string(unsigned, unsigned, bool)> fn;
    return fn;
}
std::function<std::filesystem::path()>& slot_config()
{
    static std::function<std::filesystem::path()> fn;
    return fn;
}
bool (*&slot_shortcut())(const std::string&, unsigned&, unsigned&)
{
    static bool (*fn)(const std::string&, unsigned&, unsigned&) = nullptr;
    return fn;
}
std::function<void(const std::string&)>& slot_ui_log()
{
    static std::function<void(const std::string&)> fn;
    return fn;
}
std::function<std::shared_ptr<void>(const std::string&, const std::string&)>& slot_caption_status()
{
    static std::function<std::shared_ptr<void>(const std::string&, const std::string&)> fn;
    return fn;
}
bool& slot_record_cli()
{
    static bool on = false;
    return on;
}

std::filesystem::path call_path(std::function<std::filesystem::path()>& slot)
{
    std::function<std::filesystem::path()> fn;
    {
        std::lock_guard<std::mutex> lock(hook_mu());
        fn = slot;
    }
    if (fn) return fn();
    return {};
}

} // namespace

void set_exe_dir(std::function<std::filesystem::path()> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_exe_dir() = std::move(fn);
}
void set_exe_path(std::function<std::filesystem::path()> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_exe_path() = std::move(fn);
}
void set_cli_path(std::function<std::filesystem::path()> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_cli_path() = std::move(fn);
}
void set_models_dir(std::function<std::filesystem::path()> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_models() = std::move(fn);
}
void set_embedding_models_dir(std::function<std::filesystem::path()> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_embed() = std::move(fn);
}
void set_log(std::function<void(int, const std::string&)> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_log() = std::move(fn);
}
void set_key_label(std::function<std::string(unsigned, unsigned, bool)> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_key() = std::move(fn);
}
void set_config_dir(std::function<std::filesystem::path()> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_config() = std::move(fn);
}
void set_parse_shortcut(bool (*fn)(const std::string&, unsigned&, unsigned&))
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_shortcut() = fn;
}

std::filesystem::path exe_dir() { return call_path(slot_exe_dir()); }
std::filesystem::path exe_path() { return call_path(slot_exe_path()); }
std::filesystem::path cli_path() { return call_path(slot_cli_path()); }
std::filesystem::path models_dir() { return call_path(slot_models()); }
std::filesystem::path embedding_models_dir() { return call_path(slot_embed()); }
std::filesystem::path config_dir() { return call_path(slot_config()); }

void log_line(int level, const std::string& message)
{
    std::function<void(int, const std::string&)> fn;
    {
        std::lock_guard<std::mutex> lock(hook_mu());
        fn = slot_log();
    }
    if (fn) fn(level, message);
    else std::cerr << message << '\n';
}
void log_info(const std::string& message) { log_line(0, message); }
void log_warn(const std::string& message) { log_line(1, message); }
void log_error(const std::string& message) { log_line(2, message); }

std::string key_label(unsigned mods, unsigned vk, bool modifiers)
{
    std::function<std::string(unsigned, unsigned, bool)> fn;
    {
        std::lock_guard<std::mutex> lock(hook_mu());
        fn = slot_key();
    }
    if (fn) return fn(mods, vk, modifiers);
    return "vk " + std::to_string(vk);
}

bool parse_shortcut(const std::string& text, unsigned& mods, unsigned& vk)
{
    bool (*fn)(const std::string&, unsigned&, unsigned&) = nullptr;
    {
        std::lock_guard<std::mutex> lock(hook_mu());
        fn = slot_shortcut();
    }
    if (!fn) return false;
    return fn(text, mods, vk);
}

void set_ui_log_line(std::function<void(const std::string&)> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_ui_log() = std::move(fn);
}

void ui_log_line(const std::string& message)
{
    std::function<void(const std::string&)> fn;
    {
        std::lock_guard<std::mutex> lock(hook_mu());
        fn = slot_ui_log();
    }
    if (fn) fn(message);
}

void set_caption_status(std::function<std::shared_ptr<void>(const std::string&, const std::string&)> fn)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_caption_status() = std::move(fn);
}

std::shared_ptr<void> caption_status(const std::string& mode, const std::string& label)
{
    std::function<std::shared_ptr<void>(const std::string&, const std::string&)> fn;
    {
        std::lock_guard<std::mutex> lock(hook_mu());
        fn = slot_caption_status();
    }
    if (!fn) return {};
    return fn(mode, label);
}

void set_record_in_cli_process(bool on)
{
    std::lock_guard<std::mutex> lock(hook_mu());
    slot_record_cli() = on;
}

bool record_in_cli_process()
{
    std::lock_guard<std::mutex> lock(hook_mu());
    return slot_record_cli();
}

} // namespace pm::video
