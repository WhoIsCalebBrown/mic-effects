// mic-effects-server — PipeWire virtual microphone and local DSP rack.
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "audio.hpp"
#include "json.hpp"
#include "server.hpp"

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> quitting{false};
void onSignal(int) { quitting = true; }

std::string homeDir() {
  const char* value = getenv("HOME");
  if (value && *value) return value;
  passwd* pw = getpwuid(getuid());
  return pw ? pw->pw_dir : "/tmp";
}

std::string xdgDir(const char* variable, const std::string& fallback) {
  const char* value = getenv(variable);
  return value && *value ? value : fallback;
}

double nowSeconds() {
  return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

bool ensurePrivateDir(const std::string& path, std::string* error) {
  if (mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) {
    if (error) *error = std::string("mkdir: ") + strerror(errno);
    return false;
  }
  struct stat st{};
  if (lstat(path.c_str(), &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
    if (error) *error = "runtime directory is not a directory owned by this user";
    return false;
  }
  chmod(path.c_str(), 0700);
  return true;
}

json eqToJson(const std::vector<EqBand>& bands) {
  json result = json::array();
  for (const EqBand& band : bands)
    result.push_back({{"on", band.on}, {"type", band.type}, {"freq", band.freq}, {"gain", band.gain}, {"q", band.q}});
  return result;
}

void eqFromJson(const json& value, std::vector<EqBand>& output) {
  if (!value.is_array()) return;
  output.clear();
  const auto& types = MicSettings::eqTypeNames();
  for (const auto& item : value) {
    if ((int)output.size() >= MicSettings::kMaxEqBands) break;
    if (!item.is_object()) continue;
    EqBand band;
    if (item.contains("on") && item["on"].is_boolean()) band.on = item["on"];
    if (item.contains("type") && item["type"].is_string()) band.type = item["type"];
    if (std::find(types.begin(), types.end(), band.type) == types.end()) band.type = "bell";
    auto number = [&](const char* key, float& target, float low, float high) {
      if (!item.contains(key) || !item[key].is_number()) return;
      float v = item[key].get<float>();
      if (std::isfinite(v)) target = std::clamp(v, low, high);
    };
    number("freq", band.freq, 20.f, 20000.f);
    number("gain", band.gain, -18.f, 18.f);
    number("q", band.q, .1f, 10.f);
    output.push_back(band);
  }
}

json settingsToJson(const MicSettings& s) {
  return {{"enabled", s.enabled}, {"voiceIsolation", s.voiceIsolation}, {"voiceIsolationIntensity", s.voiceIsolationIntensity},
          {"noiseGate", s.noiseGate}, {"noiseGateIntensity", s.noiseGateIntensity},
          {"autoLevel", s.autoLevel}, {"autoLevelIntensity", s.autoLevelIntensity},
          {"glueComp", s.glueComp}, {"glueCompIntensity", s.glueCompIntensity},
          {"deEsser", s.deEsser}, {"deEsserIntensity", s.deEsserIntensity},
          {"humFilter", s.humFilter}, {"highPass", s.highPass}, {"volume", s.volume},
          {"eq", eqToJson(s.eq)}, {"chain", s.chain}, {"voice", s.voice}, {"space", s.space},
          {"tape", s.tape}, {"ringMod", s.ringMod}, {"megaphone", s.megaphone},
          {"tapeMix", s.tapeMix}, {"ringModMix", s.ringModMix}, {"megaphoneMix", s.megaphoneMix},
          {"pitch", s.pitch}, {"formant", s.formant}, {"doubler", s.doubler}, {"doublerMix", s.doublerMix},
          {"slapDelay", s.slapDelay}, {"slapDelayMix", s.slapDelayMix}, {"slapDelayTime", s.slapDelayTime},
          {"longDelay", s.longDelay}, {"longDelayMix", s.longDelayMix}, {"longDelayTime", s.longDelayTime},
          {"longDelayFeedback", s.longDelayFeedback}, {"longDelayTone", s.longDelayTone},
          {"compMix", s.compMix}, {"pitchMix", s.pitchMix}, {"voiceMix", s.voiceMix}, {"spaceMix", s.spaceMix},
          {"spaceSize", s.spaceSize}, {"spaceDecay", s.spaceDecay}, {"spaceTone", s.spaceTone},
          {"spacePreDelay", s.spacePreDelay}, {"spaceMod", s.spaceMod}, {"spaceDiffusion", s.spaceDiffusion},
          {"spaceLowCut", s.spaceLowCut}, {"spaceModRate", s.spaceModRate}, {"spaceModDepth", s.spaceModDepth},
          {"autoTune", s.autoTune}, {"autoTuneSpeed", s.autoTuneSpeed}, {"autoTuneAmount", s.autoTuneAmount},
          {"autoTuneKey", s.autoTuneKey}, {"autoTuneScale", s.autoTuneScale}};
}

void settingsFromJson(const json& value, MicSettings& s) {
  if (!value.is_object()) return;
  auto boolean = [&](const char* key, bool& target) {
    if (value.contains(key) && value[key].is_boolean()) target = value[key];
  };
  auto unit = [&](const char* key, float& target) {
    if (!value.contains(key) || !value[key].is_number()) return;
    float v = value[key].get<float>();
    if (std::isfinite(v)) target = std::clamp(v, 0.f, 1.f);
  };
  auto semitone = [&](const char* key, float& target) {
    if (!value.contains(key) || !value[key].is_number()) return;
    float v = value[key].get<float>();
    if (std::isfinite(v)) target = std::clamp(v, -12.f, 12.f);
  };
  auto string = [&](const char* key, std::string& target) {
    if (value.contains(key) && value[key].is_string()) target = value[key];
  };
  boolean("enabled", s.enabled);
  boolean("voiceIsolation", s.voiceIsolation); unit("voiceIsolationIntensity", s.voiceIsolationIntensity);
  boolean("noiseGate", s.noiseGate); unit("noiseGateIntensity", s.noiseGateIntensity);
  boolean("autoLevel", s.autoLevel); unit("autoLevelIntensity", s.autoLevelIntensity);
  boolean("glueComp", s.glueComp); unit("glueCompIntensity", s.glueCompIntensity);
  boolean("deEsser", s.deEsser); unit("deEsserIntensity", s.deEsserIntensity);
  boolean("highPass", s.highPass); boolean("humFilter", s.humFilter); unit("volume", s.volume);
  string("voice", s.voice); string("space", s.space);
  if (value.contains("eq")) eqFromJson(value["eq"], s.eq);
  if (value.contains("chain") && value["chain"].is_array()) {
    const auto& names = MicSettings::stageNames();
    std::vector<std::string> chain;
    for (const auto& item : value["chain"]) {
      if (!item.is_string()) continue;
      std::string id = item;
      if (std::find(names.begin(), names.end(), id) != names.end() && std::find(chain.begin(), chain.end(), id) == chain.end()) chain.push_back(id);
    }
    if (!chain.empty()) for (const auto& id : names)
      if (std::find(chain.begin(), chain.end(), id) == chain.end()) chain.push_back(id);
    s.chain = chain;
  }
  boolean("tape", s.tape); boolean("ringMod", s.ringMod); boolean("megaphone", s.megaphone);
  unit("tapeMix", s.tapeMix); unit("ringModMix", s.ringModMix); unit("megaphoneMix", s.megaphoneMix);
  semitone("pitch", s.pitch); semitone("formant", s.formant); boolean("doubler", s.doubler); unit("doublerMix", s.doublerMix);
  boolean("slapDelay", s.slapDelay); unit("slapDelayMix", s.slapDelayMix); unit("slapDelayTime", s.slapDelayTime);
  boolean("longDelay", s.longDelay); unit("longDelayMix", s.longDelayMix); unit("longDelayTime", s.longDelayTime);
  unit("longDelayFeedback", s.longDelayFeedback); unit("longDelayTone", s.longDelayTone);
  unit("compMix", s.compMix); unit("pitchMix", s.pitchMix); unit("voiceMix", s.voiceMix); unit("spaceMix", s.spaceMix);
  unit("spaceSize", s.spaceSize); unit("spaceDecay", s.spaceDecay); unit("spaceTone", s.spaceTone);
  unit("spacePreDelay", s.spacePreDelay); unit("spaceMod", s.spaceMod); unit("spaceDiffusion", s.spaceDiffusion);
  unit("spaceLowCut", s.spaceLowCut); unit("spaceModRate", s.spaceModRate);
  bool hadModDepth = value.contains("spaceModDepth");
  unit("spaceModDepth", s.spaceModDepth);
  if (!hadModDepth && value.contains("spaceMod")) s.spaceModDepth = s.spaceMod;
  boolean("autoTune", s.autoTune); unit("autoTuneSpeed", s.autoTuneSpeed); unit("autoTuneAmount", s.autoTuneAmount);
  string("autoTuneKey", s.autoTuneKey); string("autoTuneScale", s.autoTuneScale);

  auto oneOf = [](const std::vector<std::string>& choices, std::string& target, const char* fallback) {
    if (std::find(choices.begin(), choices.end(), target) == choices.end()) target = fallback;
  };
  oneOf(MicSettings::voiceNames(), s.voice, "none");
  if (s.voice == "tape") { s.tape = true; if (!value.contains("tapeMix")) s.tapeMix = s.voiceMix; }
  if (s.voice == "ringmod") { s.ringMod = true; if (!value.contains("ringModMix")) s.ringModMix = s.voiceMix; }
  if (s.voice == "megaphone") { s.megaphone = true; if (!value.contains("megaphoneMix")) s.megaphoneMix = s.voiceMix; }
  s.voice = "none";
  oneOf(MicSettings::spaceNames(), s.space, "none");
  oneOf(MicSettings::keyNames(), s.autoTuneKey, "c");
  oneOf(MicSettings::scaleNames(), s.autoTuneScale, "chromatic");
}

struct HiddenMics {
  bool all = false;
  std::vector<std::string> nodes;
  bool operator!=(const HiddenMics& other) const { return all != other.all || nodes != other.nodes; }
};

HiddenMics readHiddenMics() {
  HiddenMics hidden;
  std::string path = xdgDir("XDG_CONFIG_HOME", homeDir() + "/.config") + "/wireplumber/wireplumber.conf.d/71-mic-effects-hide-mics.conf";
  std::ifstream input(path);
  std::string line;
  bool inNodes = false;
  while (std::getline(input, line)) {
    size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line.compare(first, 3, "all") == 0) {
      size_t equals = line.find('=', first + 3);
      if (equals != std::string::npos && line.find("true", equals) != std::string::npos) hidden.all = true;
    }
    if (line.find("nodes") != std::string::npos && line.find('[') != std::string::npos) inNodes = true;
    if (inNodes) {
      size_t a = line.find('"');
      size_t b = a == std::string::npos ? a : line.find('"', a + 1);
      if (b != std::string::npos) hidden.nodes.push_back(line.substr(a + 1, b - a - 1));
      if (line.find(']') != std::string::npos) inNodes = false;
    }
  }
  return hidden;
}

struct Config {
  std::string path;
  std::string label = "Microphone Effects";
  std::string source;
  std::string resolved;
  bool muted = false;
  bool listen = true;
  bool sameForAll = true;
  MicSettings settings;
  std::map<std::string, MicSettings> settingsBySource;
};

void loadConfig(Config& config) {
  std::ifstream input(config.path);
  if (!input) return;
  try {
    json value = json::parse(input);
    if (value.contains("label") && value["label"].is_string()) config.label = value["label"];
    if (value.contains("source") && value["source"].is_string()) config.source = value["source"];
    if (value.contains("resolved") && value["resolved"].is_string()) config.resolved = value["resolved"];
    if (value.contains("muted") && value["muted"].is_boolean()) config.muted = value["muted"];
    if (value.contains("listen") && value["listen"].is_boolean()) config.listen = value["listen"];
    if (value.contains("sameForAll") && value["sameForAll"].is_boolean()) config.sameForAll = value["sameForAll"];
    if (value.contains("settings")) settingsFromJson(value["settings"], config.settings);
    if (value.contains("settingsBySource") && value["settingsBySource"].is_object())
      for (auto& [name, item] : value["settingsBySource"].items()) {
        MicSettings settings = config.settings;
        settingsFromJson(item, settings);
        config.settingsBySource[name] = settings;
      }
  } catch (const std::exception& e) {
    fprintf(stderr, "mic-effects-server: config: %s\n", e.what());
  }
}

void saveConfig(const Config& config) {
  json bySource = json::object();
  for (const auto& [name, settings] : config.settingsBySource) bySource[name] = settingsToJson(settings);
  json value = {{"label", config.label}, {"source", config.source}, {"resolved", config.resolved},
                {"muted", config.muted}, {"listen", config.listen}, {"sameForAll", config.sameForAll},
                {"settings", settingsToJson(config.settings)}, {"settingsBySource", bySource}};
  std::string directory = config.path.substr(0, config.path.rfind('/'));
  std::error_code directoryError;
  std::filesystem::create_directories(directory, directoryError);
  if (directoryError) return;
  std::string temporary = config.path + ".tmp";
  { std::ofstream output(temporary); output << value.dump(2) << "\n"; }
  chmod(temporary.c_str(), 0600);
  rename(temporary.c_str(), config.path.c_str());
}

class Daemon {
 public:
  explicit Daemon(Config config) : config_(std::move(config)) {}

  int run() {
    std::string runtime = xdgDir("XDG_RUNTIME_DIR", "/tmp") + "/mic-effects";
    std::string error;
    if (!ensurePrivateDir(runtime, &error)) { fprintf(stderr, "mic-effects-server: %s\n", error.c_str()); return 1; }
    int lock = open((runtime + "/lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { fprintf(stderr, "mic-effects-server: another instance is running\n"); return 3; }
    if (!server_.start(runtime + "/ctl.sock", [this](int c, const std::string& r) { return handle(c, r); },
                       [this](int c) { clientClosed(c); }, &error)) {
      fprintf(stderr, "mic-effects-server: control socket: %s\n", error.c_str());
      return 1;
    }
    if (!audio_.start(config_.label, &error)) fprintf(stderr, "mic-effects-server: %s\n", error.c_str());
    {
      std::lock_guard<std::mutex> guard(mutex_);
      status_ = audio_.started() ? "ok" : error;
      hidden_ = readHiddenMics();
      pushSettings();
    }
    fprintf(stderr, "mic-effects-server: ready (socket %s/ctl.sock)\n", runtime.c_str());
    auto last = Clock::now();
    while (!quitting) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      auto now = Clock::now();
      if (now - last < std::chrono::milliseconds(250)) continue;
      last = now;
      std::string newStatus = audio_.maintain(nowSeconds());
      HiddenMics newHidden = readHiddenMics();
      bool publish = dirty_.exchange(false);
      {
        std::lock_guard<std::mutex> guard(mutex_);
        if (newStatus != status_) { status_ = newStatus; publish = true; }
        if (newHidden != hidden_) { hidden_ = std::move(newHidden); publish = true; }
        std::string selected = selectedSource();
        if (selected != lastSource_ || effectiveSettings() != lastSettings_) { pushSettings(); publish = true; }
        if (monitor_) publish = true;
        if (restart_) {
          restart_ = false;
          audio_.start(config_.label, &error);
          status_ = audio_.started() ? "ok" : error;
          pushSettings();
          publish = true;
        }
      }
      if (publish) server_.broadcast(stateJson().dump());
    }
    audio_.stop();
    server_.stop();
    close(lock);
    return 0;
  }

 private:
  std::string selectedSource() {
    if (!config_.source.empty()) return config_.source;
    std::string current = audio_.currentSource().name;
    if (!current.empty() && current != config_.resolved) { config_.resolved = current; saveConfig(config_); }
    return config_.resolved;
  }

  MicSettings& effectiveSettings(bool create = false) {
    std::string name = selectedSource();
    if (config_.sameForAll || name.empty()) return config_.settings;
    auto found = config_.settingsBySource.find(name);
    if (found == config_.settingsBySource.end()) {
      if (!create) return config_.settings;
      found = config_.settingsBySource.emplace(name, config_.settings).first;
    }
    return found->second;
  }

  void pushSettings() {
    audio_.setMuted(config_.muted);
    audio_.setListen(config_.listen && monitor_);
    audio_.setSource(config_.source);
    audio_.setSettings(effectiveSettings());
    lastSource_ = selectedSource();
    lastSettings_ = effectiveSettings();
  }

  bool hidden(const std::string& name) const {
    return hidden_.all || std::find(hidden_.nodes.begin(), hidden_.nodes.end(), name) != hidden_.nodes.end();
  }

  json stateJson() {
    std::lock_guard<std::mutex> guard(mutex_);
    json sources = json::array();
    for (const auto& source : audio_.sources())
      sources.push_back({{"name", source.name}, {"description", source.description}, {"bluetooth", source.bluetooth}, {"hidden", hidden(source.name)}});
    MicSourceInfo source = audio_.currentSource();
    json mic = {{"label", config_.label}, {"node", AudioEngine::kNodeName},
                {"source", {{"name", source.name}, {"description", source.description}, {"bluetooth", source.bluetooth}}},
                {"sources", sources}, {"wanted", config_.source}, {"status", status_},
                {"active", audio_.active()}, {"consumers", audio_.consumers()}, {"capturing", audio_.capturing()},
                {"muted", config_.muted}, {"listen", config_.listen}, {"listening", audio_.listening()},
                {"inLevel", audio_.inLevel()}, {"outLevel", audio_.outLevel()}, {"rate", audio_.rate()},
                {"hideAll", hidden_.all}, {"sameForAll", config_.sameForAll},
                {"settings", settingsToJson(effectiveSettings())}, {"eqTypes", MicSettings::eqTypeNames()},
                {"stages", MicSettings::stageNames()}, {"voices", MicSettings::voiceNames()},
                {"spaces", MicSettings::spaceNames()}, {"tuneKeys", MicSettings::keyNames()}, {"tuneScales", MicSettings::scaleNames()}};
    return {{"type", "state"}, {"mic", mic}};
  }

  void clientClosed(int client) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!meterClients_.erase(client)) return;
    monitor_ = !meterClients_.empty();
    audio_.setMonitor(monitor_);
    audio_.setListen(config_.listen && monitor_);
    dirty_ = true;
  }

  std::string handle(int client, const std::string& request) {
    static const std::string ok = R"({"type":"ok"})";
    auto failure = [](const char* message) { return json{{"type", "error"}, {"error", message}}.dump(); };
    json value = json::parse(request, nullptr, false);
    if (!value.is_object() || !value.contains("cmd") || !value["cmd"].is_string()) return failure("missing cmd");
    std::string command = value["cmd"];
    if (command == "get" || command == "status") return stateJson().dump();
    if (command == "quit") { quitting = true; return ok; }
    std::lock_guard<std::mutex> guard(mutex_);
    if (command == "micpreview") {
      bool on = value.contains("on") && value["on"].is_boolean() && value["on"];
      if (on) meterClients_.insert(client); else meterClients_.erase(client);
      monitor_ = !meterClients_.empty();
      audio_.setMonitor(monitor_);
      audio_.setListen(config_.listen && monitor_);
      dirty_ = true;
      return ok;
    }
    if (command == "micreset") {
      effectiveSettings(true) = MicSettings();
      pushSettings();
      saveConfig(config_);
      dirty_ = true;
      return ok;
    }
    if (command != "set" || !value.contains("mic") || !value["mic"].is_object()) return failure("unknown command");
    const json& mic = value["mic"];
    if (mic.contains("sameForAll") && mic["sameForAll"].is_boolean()) {
      bool same = mic["sameForAll"];
      if (!same && config_.sameForAll && !selectedSource().empty()) config_.settingsBySource[selectedSource()] = config_.settings;
      if (same && !config_.sameForAll) config_.settings = effectiveSettings();
      config_.sameForAll = same;
    }
    if (mic.contains("label") && mic["label"].is_string()) {
      std::string label = mic["label"];
      bool printable = !label.empty() && label.size() <= 64 && std::all_of(label.begin(), label.end(), [](unsigned char c) { return c >= 0x20 && c != 0x7f; });
      if (printable && label != config_.label) { config_.label = label; restart_ = true; }
    }
    if (mic.contains("source") && mic["source"].is_string()) config_.source = mic["source"];
    if (mic.contains("muted") && mic["muted"].is_boolean()) config_.muted = mic["muted"];
    if (mic.contains("listen") && mic["listen"].is_boolean()) config_.listen = mic["listen"];
    if (mic.contains("settings") && mic["settings"].is_object()) settingsFromJson(mic["settings"], effectiveSettings(true));
    pushSettings();
    saveConfig(config_);
    dirty_ = true;
    return ok;
  }

  Config config_;
  AudioEngine audio_;
  ControlServer server_;
  std::mutex mutex_;
  std::set<int> meterClients_;
  bool monitor_ = false;
  bool restart_ = false;
  std::string status_ = "starting";
  HiddenMics hidden_;
  std::string lastSource_;
  MicSettings lastSettings_;
  std::atomic<bool> dirty_{true};
};

int client(const std::string& socketPath, const json& request) {
  sockaddr_un address{};
  if (socketPath.size() >= sizeof(address.sun_path)) return 1;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
  if (connect(fd, (sockaddr*)&address, sizeof address) < 0) { fprintf(stderr, "mic-effects-server: daemon not running\n"); return 1; }
  std::string message = request.dump() + "\n";
  send(fd, message.data(), message.size(), MSG_NOSIGNAL);
  timeval timeout{5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  std::string buffer;
  char chunk[8192];
  while (true) {
    ssize_t count = recv(fd, chunk, sizeof chunk, 0);
    if (count <= 0) break;
    buffer.append(chunk, count);
    size_t newline = buffer.find('\n');
    if (newline != std::string::npos) { printf("%s\n", buffer.substr(0, newline).c_str()); close(fd); return 0; }
  }
  close(fd);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string command = argc > 1 ? argv[1] : "run";
  std::string runtime = xdgDir("XDG_RUNTIME_DIR", "/tmp") + "/mic-effects/ctl.sock";
  if (command == "status") return client(runtime, {{"cmd", "get"}});
  if (command == "quit") return client(runtime, {{"cmd", "quit"}});
  if (command != "run") {
    fprintf(stderr, "usage: mic-effects-server [run|status|quit]\n");
    return 2;
  }
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGPIPE, SIG_IGN);
  Config config;
  config.path = xdgDir("XDG_CONFIG_HOME", homeDir() + "/.config") + "/mic-effects/config.json";
  loadConfig(config);
  Daemon daemon(std::move(config));
  return daemon.run();
}
