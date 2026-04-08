#include <curl/curl.h>
#include <boost/dll/alias.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>

#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../../trunk-recorder/call_concluder/call_concluder.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Whisper_Transcribe_System {
  std::string short_name;
  bool enabled = true;
  std::string language;
  std::string prompt;
  bool include_segments = true;

  std::vector<boost::regex> tg_allow;
  std::vector<boost::regex> tg_deny;
  std::vector<std::string> tg_allow_raw;
  std::vector<std::string> tg_deny_raw;
};

struct Whisper_Transcribe_Data {
  std::string server;
  std::string api_key;
  std::string model;
  std::string response_format;
  int timeout_seconds = 300;
  std::string audio_source = "wav";

  std::vector<Whisper_Transcribe_System> systems;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  ((std::string *)userp)->append((char *)contents, size * nmemb);
  return size * nmemb;
}

class Whisper_Transcribe : public Plugin_Api {
  Whisper_Transcribe_Data data;
  std::string plugin_name;
  std::string log_prefix = "\t[Whisper Transcribe]\t";

private:
  void log_plugin_info(const std::string &msg) const {
    BOOST_LOG_TRIVIAL(info) << log_prefix << msg;
  }

  void log_plugin_warn(const std::string &msg) const {
    BOOST_LOG_TRIVIAL(warning) << log_prefix << "\033[0;33m" << msg << "\033[0m";
  }

  void log_plugin_error(const std::string &msg) const {
    BOOST_LOG_TRIVIAL(error) << log_prefix << "\033[0;31m" << msg << "\033[0m";
  }

  std::string build_loghdr(const Call_Data_t &call_info) const {
    return log_header(call_info.short_name,
                      call_info.call_num,
                      call_info.talkgroup_display,
                      call_info.freq);
  }

  void log_call_info(const Call_Data_t &call_info, const std::string &msg) const {
    BOOST_LOG_TRIVIAL(info) << build_loghdr(call_info) << msg;
  }

  void log_call_warn(const Call_Data_t &call_info, const std::string &msg) const {
    BOOST_LOG_TRIVIAL(warning) << build_loghdr(call_info)
                               << "\033[0;33m" << msg << "\033[0m";
  }

  void log_call_error(const Call_Data_t &call_info, const std::string &msg) const {
    BOOST_LOG_TRIVIAL(error) << build_loghdr(call_info)
                             << "\033[0;31m" << msg << "\033[0m";
  }

  static std::string bool_to_string(bool value) {
    return value ? "true" : "false";
  }

  static std::string glob_to_regex_str(const std::string &glob) {
    std::string rx;
    rx.reserve(glob.size() * 2);
    rx += "^";

    for (char c : glob) {
      switch (c) {
        case '*': rx += ".*"; break;
        case '?': rx += "."; break;

        case '.': case '+': case '(': case ')': case '^': case '$':
        case '|': case '{': case '}': case '[': case ']': case '\\':
          rx += "\\";
          rx += c;
          break;

        default:
          rx += c;
          break;
      }
    }

    rx += "$";
    return rx;
  }

  static bool match_any(const std::string &value, const std::vector<boost::regex> &patterns) {
    for (const auto &r : patterns) {
      if (boost::regex_match(value, r)) {
        return true;
      }
    }
    return false;
  }

  static bool passes_talkgroup_filter(const Whisper_Transcribe_System *sys, uint32_t talkgroup) {
    if (!sys) return true;

    const std::string tg = std::to_string(talkgroup);

    if (!sys->tg_allow.empty() && !match_any(tg, sys->tg_allow)) {
      return false;
    }

    if (!sys->tg_deny.empty() && match_any(tg, sys->tg_deny)) {
      return false;
    }

    return true;
  }

  static void compile_patterns_from_json(
      const json &parent,
      const char *key,
      std::vector<boost::regex> &out_compiled,
      std::vector<std::string> &out_raw,
      const std::string &log_prefix,
      const std::string &sys_short_name) {
    out_compiled.clear();
    out_raw.clear();

    if (!parent.contains(key)) return;
    const auto &j = parent.at(key);

    if (!j.is_array()) {
      BOOST_LOG_TRIVIAL(error) << log_prefix << "\033[0;31m"
                               << sys_short_name << " " << key
                               << " must be an array"
                               << "\033[0m";
      return;
    }

    for (const auto &v : j) {
      std::string pat;

      if (v.is_string()) {
        pat = v.get<std::string>();
      } else if (v.is_number_unsigned() || v.is_number_integer()) {
        pat = std::to_string(v.get<long long>());
      } else {
        continue;
      }

      boost::algorithm::trim(pat);
      if (pat.empty()) continue;

      try {
        out_raw.push_back(pat);
        out_compiled.emplace_back(glob_to_regex_str(pat));
      } catch (const boost::regex_error &e) {
        BOOST_LOG_TRIVIAL(error) << log_prefix << "\033[0;31m"
                                 << sys_short_name
                                 << " invalid pattern in " << key
                                 << " value='" << pat << "' : " << e.what()
                                 << "\033[0m";
      }
    }
  }

  static bool is_whisper_1(const std::string &model) {
    return model == "whisper-1";
  }

  static bool is_gpt4o_transcribe(const std::string &model) {
    return model == "gpt-4o-transcribe" || model == "gpt-4o-mini-transcribe";
  }

  static bool is_gpt4o_diarize(const std::string &model) {
    return model == "gpt-4o-transcribe-diarize";
  }

  static bool is_known_openai_model(const std::string &model) {
    return is_whisper_1(model) || is_gpt4o_transcribe(model) || is_gpt4o_diarize(model);
  }

  static std::string default_response_format_for_model(const std::string &model) {
    if (is_whisper_1(model)) {
      return "verbose_json";
    }
    if (is_gpt4o_transcribe(model)) {
      return "json";
    }
    if (is_gpt4o_diarize(model)) {
      return "diarized_json";
    }

    // Preserve backward compatibility for local/custom Whisper-like servers.
    return "verbose_json";
  }

  static bool response_format_supported_for_model(const std::string &model,
                                                  const std::string &response_format) {
    if (is_whisper_1(model)) {
      return response_format == "json" ||
             response_format == "text" ||
             response_format == "srt" ||
             response_format == "verbose_json" ||
             response_format == "vtt";
    }

    if (is_gpt4o_transcribe(model)) {
      // Docs are a bit inconsistent about text; json is the safest option.
      return response_format == "json" || response_format == "text";
    }

    if (is_gpt4o_diarize(model)) {
      return response_format == "json" ||
             response_format == "text" ||
             response_format == "diarized_json";
    }

    // Unknown local/custom models: trust the caller.
    return true;
  }

  static bool prompt_supported_for_model(const std::string &model) {
    return !is_gpt4o_diarize(model);
  }

  static bool response_is_json_format(const std::string &response_format) {
    return response_format == "json" ||
           response_format == "verbose_json" ||
           response_format == "diarized_json";
  }

public:
  static boost::shared_ptr<Whisper_Transcribe> create() {
    return boost::shared_ptr<Whisper_Transcribe>(new Whisper_Transcribe());
  }

  Whisper_Transcribe_System *get_system(const std::string &short_name) {
    for (auto &sys : data.systems) {
      if (sys.short_name == short_name) {
        return &sys;
      }
    }
    return nullptr;
  }

  int parse_config(json config_data) override {
    plugin_name = config_data.value("name", "whisper_transcribe");

    data.server = config_data.value("server", "");
    data.api_key = config_data.value("apiKey", "");
    data.model = config_data.value("model", "whisper-1");
    data.audio_source = config_data.value("audioSource", "wav");
    data.timeout_seconds = config_data.value("timeoutSeconds", 300);

    if (config_data.contains("responseFormat")) {
      data.response_format = config_data.value("responseFormat", "");
    } else {
      data.response_format = "";
    }

    boost::algorithm::trim(data.audio_source);
    boost::algorithm::to_lower(data.audio_source);

    if (data.audio_source != "wav" && data.audio_source != "m4a") {
      log_plugin_warn("Invalid audioSource '" + data.audio_source + "'; falling back to wav");
      data.audio_source = "wav";
    }

    boost::algorithm::trim(data.server);
    boost::algorithm::trim(data.api_key);
    boost::algorithm::trim(data.model);
    boost::algorithm::trim(data.response_format);
    boost::algorithm::to_lower(data.response_format);

    if (data.server.empty()) {
      log_plugin_error("server is required");
      return 1;
    }

    if (data.response_format.empty()) {
      data.response_format = default_response_format_for_model(data.model);
    }

    if (!response_format_supported_for_model(data.model, data.response_format)) {
      const std::string fallback = default_response_format_for_model(data.model);

      if (is_known_openai_model(data.model)) {
        log_plugin_warn("responseFormat '" + data.response_format +
                        "' is not supported for model '" + data.model +
                        "'; falling back to '" + fallback + "'");
        data.response_format = fallback;
      } else {
        log_plugin_warn("responseFormat '" + data.response_format +
                        "' may not be supported by custom model '" + data.model +
                        "'; keeping configured value");
      }
    }

    if (!config_data.contains("systems") || !config_data["systems"].is_array()) {
      log_plugin_error("systems array is required");
      return 1;
    }

    for (json element : config_data["systems"]) {
      Whisper_Transcribe_System sys;
      sys.short_name = element.value("shortName", "");
      sys.enabled = element.value("enabled", true);
      sys.language = element.value("language", "");
      sys.prompt = element.value("prompt", "");
      sys.include_segments = element.value("includeSegments", true);

      boost::algorithm::trim(sys.short_name);
      boost::algorithm::trim(sys.language);
      boost::algorithm::trim(sys.prompt);

      if (!prompt_supported_for_model(data.model) && !sys.prompt.empty()) {
        log_plugin_warn("Prompt is not supported for model '" + data.model +
                        "'; ignoring prompt for system " + sys.short_name);
        sys.prompt.clear();
      }

      compile_patterns_from_json(
          element, "talkgroupAllow",
          sys.tg_allow, sys.tg_allow_raw,
          log_prefix, sys.short_name);

      compile_patterns_from_json(
          element, "talkgroupDeny",
          sys.tg_deny, sys.tg_deny_raw,
          log_prefix, sys.short_name);

      if (sys.short_name.empty()) {
        log_plugin_error("systems entry missing shortName");
        return 1;
      }

      if (sys.enabled) {
        log_plugin_info("Configured System:      " + sys.short_name);
        log_plugin_info("  Include Segments:    " + bool_to_string(sys.include_segments));
        log_plugin_info("  Language:            " + (sys.language.empty() ? "[auto]" : sys.language));
        log_plugin_info("  Prompt:              " + (sys.prompt.empty() ? "[none]" : sys.prompt));

        if (!sys.tg_allow_raw.empty() || !sys.tg_deny_raw.empty()) {
          log_plugin_info("  Talkgroup Filters:   allow=" +
                          std::to_string(sys.tg_allow_raw.size()) +
                          " deny=" +
                          std::to_string(sys.tg_deny_raw.size()));
        }

        data.systems.push_back(sys);
      } else {
        log_plugin_info("Configured System:      " + sys.short_name + " [disabled]");
      }
    }

    if (data.systems.empty()) {
      log_plugin_error("No enabled systems configured");
      return 1;
    }

    log_plugin_info("Server:                 " + data.server);
    log_plugin_info("Model:                  " + data.model);
    log_plugin_info("Response Format:        " + data.response_format);
    log_plugin_info("Audio Source:           " + data.audio_source);
    log_plugin_info("Timeout Seconds:        " + std::to_string(data.timeout_seconds));
    log_plugin_info("Enabled Systems:        " + std::to_string(data.systems.size()));

    return 0;
  }

  int init(Config *config, std::vector<Source *> sources, std::vector<System *> systems) override {
    (void)sources;
    frequency_format = config->frequency_format;

    for (auto &cfg_sys : data.systems) {
      bool found = false;

      for (auto *sys : systems) {
        if (sys && sys->get_short_name() == cfg_sys.short_name) {
          found = true;
          break;
        }
      }

      if (!found) {
        log_plugin_error("Configured shortName not found in loaded systems: " + cfg_sys.short_name);
        return 1;
      }

      log_plugin_info("Validated System:       " + cfg_sys.short_name);
    }

    return 0;
  }

  int request_transcript(const Call_Data_t &call_info,
                         const Whisper_Transcribe_System &sys_cfg,
                         nlohmann::ordered_json &plugin_ctx) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      log_call_error(call_info, "Whisper Transcribe failed to initialize CURL");
      return 1;
    }

    std::string response_buffer;
    char curl_errbuf[CURL_ERROR_SIZE];
    curl_errbuf[0] = '\0';

    std::string audio_path = call_info.filename;
    std::string audio_source_used = "wav";

    if (data.audio_source == "m4a") {
      if (call_info.compress_wav && !call_info.converted.empty()) {
        audio_path = call_info.converted;
        audio_source_used = "m4a";
      } else {
        log_call_warn(call_info,
                      "Whisper Transcribe audioSource is set to m4a, but no converted file is available; "
                      "falling back to wav");
      }
    }

    plugin_ctx["audio_source_requested"] = data.audio_source;
    plugin_ctx["audio_source_used"] = audio_source_used;
    plugin_ctx["audio_path"] = audio_path;
    plugin_ctx["response_format"] = data.response_format;
    plugin_ctx["model"] = data.model;

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = nullptr;

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, audio_path.c_str());

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "model");
    curl_mime_data(part, data.model.c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "response_format");
    curl_mime_data(part, data.response_format.c_str(), CURL_ZERO_TERMINATED);

    if (!sys_cfg.language.empty()) {
      part = curl_mime_addpart(mime);
      curl_mime_name(part, "language");
      curl_mime_data(part, sys_cfg.language.c_str(), CURL_ZERO_TERMINATED);
    }

    if (!sys_cfg.prompt.empty()) {
      part = curl_mime_addpart(mime);
      curl_mime_name(part, "prompt");
      curl_mime_data(part, sys_cfg.prompt.c_str(), CURL_ZERO_TERMINATED);
    }

    struct curl_slist *headers = nullptr;
    if (!data.api_key.empty()) {
      std::string auth_header = "Authorization: Bearer " + data.api_key;
      headers = curl_slist_append(headers, auth_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, data.server.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(data.timeout_seconds) * 1000L);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || response_code < 200 || response_code >= 300) {
      std::ostringstream err;
      err << "Whisper Transcribe transcript request failed (HTTP " << response_code << ")";
      if (!response_buffer.empty()) {
        err << " response=" << response_buffer;
      }
      if (curl_errbuf[0]) {
        err << " curl_err=" << curl_errbuf;
      }
      log_call_error(call_info, err.str());
      return 1;
    }

    plugin_ctx["transcript"] = "";
    plugin_ctx["segments"] = nlohmann::ordered_json::array();

    if (response_is_json_format(data.response_format)) {
      nlohmann::json parsed = nlohmann::json::parse(response_buffer, nullptr, false);
      if (parsed.is_discarded()) {
        log_call_error(call_info,
                       "Whisper Transcribe transcript response was not valid JSON for responseFormat=" +
                       data.response_format);
        return 1;
      }

      plugin_ctx["transcript"] = parsed.value("text", "");
      plugin_ctx["raw_response"] = parsed;

      if (sys_cfg.include_segments &&
          parsed.contains("segments") &&
          parsed["segments"].is_array()) {
        for (const auto &seg : parsed["segments"]) {
          nlohmann::ordered_json seg_json;
          seg_json["id"] = seg.value("id", 0);
          seg_json["start"] = seg.value("start", 0.0);
          seg_json["end"] = seg.value("end", 0.0);
          seg_json["text"] = seg.value("text", "");
          if (seg.contains("speaker")) {
            seg_json["speaker"] = seg["speaker"];
          }
          plugin_ctx["segments"].push_back(seg_json);
        }
      }
    } else {
      // text / srt / vtt
      plugin_ctx["transcript"] = response_buffer;
    }

    log_call_info(call_info,
                  "Whisper Transcribe transcript success"
                  " source=" + audio_source_used +
                  " responseFormat=" + data.response_format +
                  " segments=" + std::to_string(plugin_ctx["segments"].size()));

    return 0;
  }

  int call_end(Call_Data_t &call_info, nlohmann::ordered_json &plugin_ctx) override {
    if (!plugin_ctx.is_object()) {
      plugin_ctx = nlohmann::ordered_json::object();
    }

    plugin_ctx["transcript"] = "";
    plugin_ctx["segments"] = nlohmann::ordered_json::array();
    plugin_ctx["process_time_seconds"] = 0.0;
    plugin_ctx["skipped"] = false;
    plugin_ctx["skip_reason"] = "";
    plugin_ctx["success"] = false;
    plugin_ctx["error"] = "";
    plugin_ctx["response_format"] = data.response_format;
    plugin_ctx["model"] = data.model;

    Whisper_Transcribe_System *sys = get_system(call_info.short_name);
    if (!sys || !sys->enabled) {
      plugin_ctx["skipped"] = true;
      plugin_ctx["skip_reason"] = "system_not_enabled";
      return 0;
    }

    if (call_info.encrypted) {
      plugin_ctx["skipped"] = true;
      plugin_ctx["skip_reason"] = "encrypted";
      return 0;
    }

    if (!passes_talkgroup_filter(sys, call_info.talkgroup)) {
      plugin_ctx["skipped"] = true;
      plugin_ctx["skip_reason"] = "talkgroup_filter";

      log_call_info(call_info,
                    "Whisper Transcribe skipped transcription due to talkgroup filter (tg=" +
                    std::to_string(call_info.talkgroup) + ")");
      return 0;
    }

    auto start = std::chrono::steady_clock::now();
    int rc = request_transcript(call_info, *sys, plugin_ctx);
    auto end = std::chrono::steady_clock::now();

    plugin_ctx["process_time_seconds"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0;

    if (rc == 0) {
      plugin_ctx["success"] = true;
    } else {
      plugin_ctx["error"] = "transcription_request_failed";
    }

    return rc;
  }
};

BOOST_DLL_ALIAS(
    Whisper_Transcribe::create,
    create_plugin
)