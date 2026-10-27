#include "fcitx5-candidate-translator/src/bridge.rs.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/event.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>

namespace {

namespace backend = candidate_translator;

constexpr char kConfigPath[] = "conf/candidate-translator.conf";
// Matches the message the Rust worker returns while it is backing off.
const std::string cooldown_error = "translation service is cooling down after an error";

enum class TargetLanguage { English, Japanese };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(TargetLanguage, N_("English"), N_("Japanese"))

enum class ReasoningEffortMode { Auto, None, Low, Medium };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(ReasoningEffortMode, N_("Auto"), N_("None"),
                                 N_("Low"), N_("Medium"))

enum class CommitMode { Off, Toggle, Always };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(CommitMode, N_("Never"),
                                 N_("When toggled on"), N_("Always"))

FCITX_CONFIGURATION(
    TranslatorConfig,
    fcitx::Option<bool> enabled{this, "Enabled", _("Enable candidate translation"), true};
    fcitx::Option<std::string> baseUrl{
        this, "BaseURL", _("OpenAI-compatible Base URL"), ""};
    fcitx::Option<std::string> model{this, "Model", _("Model"), ""};
    fcitx::Option<std::string> apiKey{this, "APIKey", _("API Key (stored as plain text)"), ""};
    fcitx::OptionWithAnnotation<TargetLanguage, TargetLanguageI18NAnnotation>
        targetLanguage{this, "TargetLanguage", _("Target language"),
                       TargetLanguage::English};
    fcitx::Option<bool> showKanaReading{
        this, "ShowKanaReading", _("Show kana reading for Japanese translations"),
        true};
    fcitx::OptionWithAnnotation<ReasoningEffortMode,
                                ReasoningEffortModeI18NAnnotation>
        reasoningEffort{this, "ReasoningEffort", _("Reasoning effort"),
                        ReasoningEffortMode::Auto};
    fcitx::Option<int, fcitx::IntConstrain> debounceMs{
        this, "DebounceMs", _("Request debounce (milliseconds)"), 180,
        fcitx::IntConstrain(0, 2000)};
    fcitx::Option<int, fcitx::IntConstrain> requestTimeoutMs{
        this, "RequestTimeoutMs", _("Request timeout (milliseconds)"), 8000,
        fcitx::IntConstrain(500, 15000)};
    fcitx::Option<int, fcitx::IntConstrain> cacheEntries{
        this, "CacheEntries", _("Maximum cached translations"), 2048,
        fcitx::IntConstrain(0, 100000)};
    fcitx::Option<bool> clearCache{
        this, "ClearCache", _("Clear translation cache on Apply"), false};
    fcitx::OptionWithAnnotation<CommitMode, CommitModeI18NAnnotation> commitMode{
        this, "CommitMode",
        _("Commit the translation instead of the typed Chinese"), CommitMode::Off};
    fcitx::KeyListOption toggleCommitKey{
        this, "ToggleCommitKey", _("Toggle translation commit"),
        fcitx::KeyList{fcitx::Key("Control+Alt+J")},
        fcitx::KeyListConstrain()};
    fcitx::KeyListOption sentenceKey{
        this, "SentenceKey", _("Translate the text before the cursor"),
        fcitx::KeyList{fcitx::Key("Control+Alt+Enter")},
        fcitx::KeyListConstrain()};
    fcitx::Option<int, fcitx::IntConstrain> sentenceMaxChars{
        this, "SentenceMaxChars", _("Maximum characters per sentence"), 120,
        fcitx::IntConstrain(10, 500)};)

struct ContextState {
    std::shared_ptr<fcitx::CandidateList> list;
    std::unordered_map<std::string, std::string> translations;
    std::string signature;
    std::uint64_t currentRequest = 0;
    // Committed Chinese, used when the client cannot report surrounding text.
    std::string committed;
    std::uint64_t sentenceRequest = 0;
};

struct PendingRequest {
    fcitx::InputContext *inputContext;
    std::shared_ptr<fcitx::CandidateList> list;
    std::string signature;
};

struct PendingSentence {
    fcitx::InputContext *inputContext;
    std::string source;
    bool replaceSurrounding = false;
};

// Commit text is replaced only when it is exactly one of the candidates that
// already holds a different translation.
bool rewriteCommit(
    const std::unordered_map<std::string, std::string> &translations,
    std::string &text) {
    const auto translation = translations.find(text);
    if (translation == translations.end() || translation->second.empty() ||
        translation->second == text) {
        return false;
    }
    text = translation->second;
    return true;
}

// Decodes the UTF-8 scalar at `offset`; malformed input becomes U+FFFD.
char32_t decodeUtf8(std::string_view text, std::size_t offset, std::size_t &size) {
    const auto byte = static_cast<unsigned char>(text[offset]);
    if (byte < 0x80) {
        size = 1;
        return byte;
    }
    std::size_t length = 0;
    char32_t value = 0;
    if ((byte & 0xE0) == 0xC0) {
        length = 2;
        value = byte & 0x1F;
    } else if ((byte & 0xF0) == 0xE0) {
        length = 3;
        value = byte & 0x0F;
    } else if ((byte & 0xF8) == 0xF0) {
        length = 4;
        value = byte & 0x07;
    } else {
        size = 1;
        return 0xFFFD;
    }
    if (offset + length > text.size()) {
        size = 1;
        return 0xFFFD;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto continuation = static_cast<unsigned char>(text[offset + i]);
        if ((continuation & 0xC0) != 0x80) {
            size = 1;
            return 0xFFFD;
        }
        value = (value << 6) | (continuation & 0x3F);
    }
    size = length;
    return value;
}

bool isHan(char32_t character) {
    return (character >= 0x3400 && character <= 0x4DBF) ||
           (character >= 0x4E00 && character <= 0x9FFF) ||
           (character >= 0xF900 && character <= 0xFAFF) ||
           (character >= 0x20000 && character <= 0x3FFFF);
}

// Characters that make up a translatable run: CJK, Latin letters, digits and
// everyday punctuation, but never a line break.
bool isSentenceChar(char32_t character) {
    if (character == U'\n' || character == U'\r' || character == U'\t') {
        return false;
    }
    if (character < 0x80) {
        if ((character >= U'a' && character <= U'z') ||
            (character >= U'A' && character <= U'Z') ||
            (character >= U'0' && character <= U'9')) {
            return true;
        }
        constexpr std::string_view ascii = " ,.!?;:'-()<>@/";
        return ascii.find(static_cast<char>(character)) !=
               std::string_view::npos;
    }
    if (character >= 0x3000 && character <= 0x303F) {
        return true;
    }
    if (character >= 0xFF01 && character <= 0xFF5E) {
        return true;
    }
    return isHan(character);
}

std::size_t sentenceCharacterCount(std::string_view text) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::size_t size = 1;
        decodeUtf8(text, offset, size);
        offset += size;
        ++count;
    }
    return count;
}

// Returns the run of translatable characters ending at `cursor` (an offset in
// characters), capped to `maxCharacters`.
std::string extractSentence(std::string_view text, unsigned int cursor,
                            int maxCharacters) {
    if (text.empty() || maxCharacters <= 0) {
        return {};
    }
    std::vector<std::size_t> offsets;
    std::vector<char32_t> points;
    offsets.reserve(text.size() + 1);
    points.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        std::size_t size = 1;
        points.push_back(decodeUtf8(text, offset, size));
        offsets.push_back(offset);
        offset += size;
    }
    offsets.push_back(text.size());
    if (cursor > points.size()) {
        cursor = static_cast<unsigned int>(points.size());
    }
    std::size_t begin = cursor;
    std::size_t count = 0;
    while (begin > 0 && count < static_cast<std::size_t>(maxCharacters)) {
        if (!isSentenceChar(points[begin - 1])) {
            break;
        }
        --begin;
        ++count;
    }
    // Leading spaces are padding, not part of the text to translate.
    while (begin < cursor && text[offsets[begin]] == ' ') {
        ++begin;
    }
    if (begin >= cursor) {
        return {};
    }
    return std::string(text.substr(offsets[begin], offsets[cursor] - offsets[begin]));
}

backend::Candidate prepareCandidate(std::uint32_t index, std::string_view source) {
    return backend::prepare_candidate(
        index, rust::Slice<const std::uint8_t>(
                   reinterpret_cast<const std::uint8_t *>(source.data()),
                   source.size()));
}

class CandidateTranslatorAddon final : public fcitx::AddonInstance {
public:
    explicit CandidateTranslatorAddon(fcitx::Instance *instance)
        : instance_(instance), translator_(backend::new_translator()) {
        resultEvent_ = instance_->eventLoop().addIOEvent(
            translator_->result_fd(), fcitx::IOEventFlag::In,
            [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
                for (auto &result : translator_->take_results()) {
                    applyResult(std::move(result));
                }
                return true;
            });
        if (!resultEvent_) {
            throw std::runtime_error("Failed to watch translator results");
        }
        reloadConfig();
        outputConnection_ = instance_->connect<fcitx::Instance::OutputFilter>(
            [this](fcitx::InputContext *inputContext, fcitx::Text &text) {
                filterOutput(inputContext, text);
            });
        commitConnection_ = instance_->connect<fcitx::Instance::CommitFilter>(
            [this](fcitx::InputContext *inputContext, std::string &text) {
                filterCommit(inputContext, text);
            });
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                handleKeyEvent(static_cast<fcitx::KeyEvent &>(event));
            }));
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputContextUpdateUI,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &event) {
                auto &update = static_cast<fcitx::InputContextUpdateUIEvent &>(event);
                if (update.component() == fcitx::UserInterfaceComponent::InputPanel) {
                    updateInputContext(update.inputContext());
                }
            }));
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputContextDestroyed,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &event) {
                auto &destroyed = static_cast<fcitx::InputContextDestroyedEvent &>(event);
                removeInputContext(destroyed.inputContext());
            }));
    }

    ~CandidateTranslatorAddon() override {
        // Unregister the borrowed fd before Translator joins its worker and
        // closes the sockets. Background work never accesses this addon.
        resultEvent_.reset();
        commitConnection_.disconnect();
        clearAll();
    }

    const fcitx::Configuration *getConfig() const override { return &config_; }

    void setConfig(const fcitx::RawConfig &rawConfig) override {
        clearAll();
        config_.load(rawConfig, true);
        if (*config_.clearCache) {
            translator_->clear_cache();
            config_.clearCache.setValue(false);
        }
        fcitx::safeSaveAsIni(config_, kConfigPath);
        configureBackend();
    }

    void reloadConfig() override {
        clearAll();
        fcitx::readAsIni(config_, kConfigPath);
        configureBackend();
    }

private:
    std::string targetLanguage() const {
        if (*config_.targetLanguage != TargetLanguage::Japanese) {
            return "English";
        }
        return *config_.showKanaReading ? "JapaneseWithKana" : "Japanese";
    }

    bool configured() const {
        return *config_.enabled && !config_.baseUrl->empty() &&
               !config_.model->empty() && !config_.apiKey->empty();
    }

    std::string reasoningEffort() const {
        switch (*config_.reasoningEffort) {
        case ReasoningEffortMode::None:
            return "none";
        case ReasoningEffortMode::Low:
            return "low";
        case ReasoningEffortMode::Medium:
            return "medium";
        case ReasoningEffortMode::Auto:
            return "";
        }
        return "";
    }

    void configureBackend() {
        const auto cachePath =
            fcitx::StandardPaths::global()
                .userDirectory(fcitx::StandardPathsType::Cache) /
            "candidate-translator/cache-v1.json";
        const auto reasoning = reasoningEffort();
        const auto dictionaryPath = fcitx::StandardPaths::global().locate(
            fcitx::StandardPathsType::PkgData,
            "candidate-translator/cedict_ts.csv");
        translator_->configure(backend::BackendConfig{
            .enabled = *config_.enabled,
            .base_url = *config_.baseUrl,
            .model = *config_.model,
            .api_key = *config_.apiKey,
            .reasoning_effort = reasoning,
            .dictionary_path = dictionaryPath.string(),
            .timeout_ms = static_cast<std::uint64_t>(*config_.requestTimeoutMs),
            .debounce_ms = static_cast<std::uint64_t>(*config_.debounceMs),
            .cache_entries = static_cast<std::size_t>(*config_.cacheEntries),
            .cache_path = cachePath.string(),
        });
        const auto configPath =
            fcitx::StandardPaths::global()
                .userDirectory(fcitx::StandardPathsType::Config) /
            kConfigPath;
        ::chmod(configPath.c_str(), 0600);
    }

    void clearAll() {
        translator_->cancel_requests();
        contexts_.clear();
        pending_.clear();
        sentences_.clear();
        commitToggled_.clear();
    }

    void removeInputContext(fcitx::InputContext *inputContext) {
        contexts_.erase(inputContext);
        commitToggled_.erase(inputContext);
        std::erase_if(pending_, [inputContext](const auto &entry) {
            return entry.second.inputContext == inputContext;
        });
        std::erase_if(sentences_, [inputContext](const auto &entry) {
            return entry.second.inputContext == inputContext;
        });
    }

    bool commitTranslationActive(fcitx::InputContext *inputContext) const {
        switch (*config_.commitMode) {
        case CommitMode::Always:
            return true;
        case CommitMode::Toggle: {
            const auto toggled = commitToggled_.find(inputContext);
            return toggled != commitToggled_.end() && toggled->second;
        }
        case CommitMode::Off:
            break;
        }
        return false;
    }

    // InputContext::commitString routes every commit through this filter, so
    // replacing the text here turns "type Chinese, accept" into English without
    // intercepting the key that accepted the candidate.
    void filterCommit(fcitx::InputContext *inputContext, std::string &text) {
        if (text.empty()) {
            return;
        }
        recordCommitted(inputContext, text);
        if (!commitTranslationActive(inputContext)) {
            return;
        }
        const auto state = contexts_.find(inputContext);
        if (state != contexts_.end()) {
            rewriteCommit(state->second.translations, text);
        }
    }

    // Keeps the text the user has already committed, for clients that do not
    // report surrounding text, where the sentence cannot be replaced in place.
    void recordCommitted(fcitx::InputContext *inputContext,
                         const std::string &text) {
        if (inputContext->capabilityFlags().testAny(
                fcitx::CapabilityFlag::PasswordOrSensitive)) {
            return;
        }
        bool hasHan = false;
        for (std::size_t offset = 0; offset < text.size();) {
            std::size_t size = 1;
            const auto character = decodeUtf8(text, offset, size);
            offset += size;
            if (isHan(character)) {
                hasHan = true;
                break;
            }
        }
        if (!hasHan) {
            return;
        }
        auto &buffer = contexts_[inputContext].committed;
        buffer += text;
        const auto limit =
            static_cast<std::size_t>(*config_.sentenceMaxChars);
        if (sentenceCharacterCount(buffer) <= limit) {
            return;
        }
        std::size_t excess = sentenceCharacterCount(buffer) - limit;
        std::size_t offset = 0;
        while (offset < buffer.size() && excess > 0) {
            std::size_t size = 1;
            decodeUtf8(buffer, offset, size);
            offset += size;
            --excess;
        }
        buffer.erase(0, offset);
    }

    void handleKeyEvent(fcitx::KeyEvent &event) {
        if (event.isRelease() || event.filtered() || event.accepted()) {
            return;
        }
        auto *inputContext = event.inputContext();
        if (!inputContext || !inputContext->hasFocus()) {
            return;
        }
        const auto &sentenceKeys = *config_.sentenceKey;
        if (!sentenceKeys.empty() && event.key().checkKeyList(sentenceKeys)) {
            event.filterAndAccept();
            translateSentence(inputContext);
            return;
        }
        if (*config_.commitMode != CommitMode::Toggle) {
            return;
        }
        const auto &keys = *config_.toggleCommitKey;
        if (keys.empty() || !event.key().checkKeyList(keys) ||
            inputContext->capabilityFlags().testAny(
                fcitx::CapabilityFlag::PasswordOrSensitive)) {
            return;
        }
        auto &active = commitToggled_[inputContext];
        active = !active;
        event.filterAndAccept();
        inputContext->updateUserInterface(
            fcitx::UserInterfaceComponent::InputPanel);
    }

    // Translates the run of text ending at the cursor and replaces it in place.
    void translateSentence(fcitx::InputContext *inputContext) {
        if (!configured() ||
            inputContext->capabilityFlags().testAny(
                fcitx::CapabilityFlag::PasswordOrSensitive)) {
            return;
        }
        auto &state = contexts_[inputContext];
        std::string source;
        if (const auto &surrounding = inputContext->surroundingText();
            surrounding.isValid()) {
            source = extractSentence(surrounding.text(), surrounding.cursor(),
                                     *config_.sentenceMaxChars);
        }
        const bool replaceSurrounding = !source.empty();
        if (!replaceSurrounding) {
            source = state.committed;
        }
        if (source.empty()) {
            return;
        }
        if (const auto cached = translator_->lookup(targetLanguage(), source);
            !cached.empty()) {
            applySentence(inputContext, source, std::string(cached),
                          replaceSurrounding);
            return;
        }
        const auto requestId = nextRequestId_++;
        if (state.sentenceRequest != 0) {
            sentences_.erase(state.sentenceRequest);
        }
        state.sentenceRequest = requestId;
        sentences_.insert_or_assign(
            requestId, PendingSentence{inputContext, source, replaceSurrounding});
        translator_->submit_text(requestId, targetLanguage(), source);
    }

    void applySentence(fcitx::InputContext *inputContext,
                       const std::string &source, std::string translation,
                       bool replaceSurrounding) {
        if (auto state = contexts_.find(inputContext); state != contexts_.end()) {
            state->second.sentenceRequest = 0;
            state->second.committed.clear();
        }
        if (replaceSurrounding) {
            const auto length =
                static_cast<unsigned int>(sentenceCharacterCount(source));
            if (length > 0) {
                inputContext->deleteSurroundingText(-static_cast<int>(length),
                                                    length);
            }
        }
        inputContext->commitString(translation);
    }

    void filterOutput(fcitx::InputContext *inputContext, fcitx::Text &text) {
        auto state = contexts_.find(inputContext);
        if (state == contexts_.end()) {
            return;
        }
        auto currentList = inputContext->inputPanel().candidateList();
        if (!currentList || currentList.get() != state->second.list.get()) {
            return;
        }
        const auto source = text.toStringForCommit();
        auto translation = state->second.translations.find(source);
        if (translation == state->second.translations.end()) {
            return;
        }
        text.append("  ");
        if (commitTranslationActive(inputContext)) {
            text.append("→ ", fcitx::TextFormatFlag::Bold);
            text.append(translation->second, fcitx::TextFormatFlag::Underline);
        } else {
            text.append(translation->second, fcitx::TextFormatFlag::Italic);
        }
    }

    void updateInputContext(fcitx::InputContext *inputContext) {
        auto list = inputContext->inputPanel().candidateList();
        auto &state = contexts_[inputContext];
        const bool sensitive = inputContext->capabilityFlags().testAny(
            fcitx::CapabilityFlag::PasswordOrSensitive);
        if (!configured() || sensitive || !list || list->empty()) {
            if (state.currentRequest != 0) {
                pending_.erase(state.currentRequest);
            }
            state = {};
            return;
        }

        if (state.list.get() != list.get()) {
            if (state.currentRequest != 0) {
                pending_.erase(state.currentRequest);
            }
            state = {};
            state.list = list;
        }

        std::string signature = targetLanguage();
        state.translations.clear();
        rust::Vec<backend::Candidate> missing;
        for (int index = 0; index < list->size(); ++index) {
            const auto &word = list->candidate(index);
            const auto source = word.text().toStringForCommit();
            signature.append("\x1f").append(source);
            if (word.isPlaceHolder()) {
                continue;
            }
            auto candidate = prepareCandidate(static_cast<std::uint32_t>(index), source);
            if (candidate.source.empty()) {
                continue;
            }
            auto cached = translator_->lookup(targetLanguage(), candidate.source);
            if (!cached.empty()) {
                state.translations.insert_or_assign(source, std::string(cached));
            } else if (missing.size() < 64) {
                missing.push_back(std::move(candidate));
            }
        }
        if (state.signature != signature && state.currentRequest != 0) {
            pending_.erase(state.currentRequest);
            state.currentRequest = 0;
        }
        state.signature = signature;
        if (missing.empty() || state.currentRequest != 0) {
            return;
        }

        const auto requestId = nextRequestId_++;
        state.currentRequest = requestId;
        pending_.emplace(requestId,
                         PendingRequest{inputContext, list, signature});
        translator_->submit(requestId, targetLanguage(), std::move(missing));
    }

    void applyResult(backend::TranslationResult result) {
        if (auto sentence = sentences_.find(result.request_id);
            sentence != sentences_.end()) {
            auto request = std::move(sentence->second);
            sentences_.erase(sentence);
            if (!result.error.empty()) {
                if (result.error != "translation request was superseded") {
                    if (result.error == cooldown_error) {
                        FCITX_DEBUG() << "Sentence translation skipped during cooldown";
                    } else {
                        FCITX_WARN() << "Sentence translation failed: "
                                     << std::string(result.error);
                    }
                }
                return;
            }
            if (result.translations.empty() ||
                result.translations[0].text.empty()) {
                return;
            }
            applySentence(request.inputContext, request.source,
                          std::string(result.translations[0].text),
                          request.replaceSurrounding);
            return;
        }
        auto pending = pending_.find(result.request_id);
        if (pending == pending_.end()) {
            return;
        }
        const auto request = std::move(pending->second);
        pending_.erase(pending);
        auto stateIter = contexts_.find(request.inputContext);
        if (stateIter == contexts_.end()) {
            return;
        }
        auto &state = stateIter->second;
        state.currentRequest = 0;
        auto currentList = request.inputContext->inputPanel().candidateList();
        if (state.signature != request.signature ||
            currentList.get() != request.list.get()) {
            return;
        }
        if (!result.error.empty()) {
            if (result.error != "translation request was superseded") {
                if (result.error == cooldown_error) {
                    FCITX_DEBUG() << "Candidate translation skipped during cooldown";
                } else {
                    FCITX_WARN() << "Candidate translation failed: "
                                 << std::string(result.error);
                }
            }
            return;
        }
        for (const auto &[index, translation] : result.translations) {
            if (index >= static_cast<std::uint32_t>(currentList->size()) ||
                translation.empty()) {
                continue;
            }
            const auto &word =
                currentList->candidate(static_cast<int>(index));
            state.translations.insert_or_assign(
                word.text().toStringForCommit(), std::string(translation));
        }
        request.inputContext->updateUserInterface(
            fcitx::UserInterfaceComponent::InputPanel);
    }

    fcitx::Instance *instance_;
    TranslatorConfig config_;
    fcitx::Connection outputConnection_;
    fcitx::Connection commitConnection_;
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>>
        handlers_;
    std::unordered_map<fcitx::InputContext *, ContextState> contexts_;
    std::unordered_map<fcitx::InputContext *, bool> commitToggled_;
    std::unordered_map<std::uint64_t, PendingRequest> pending_;
    std::unordered_map<std::uint64_t, PendingSentence> sentences_;
    std::uint64_t nextRequestId_ = 1;
    rust::Box<backend::Translator> translator_;
    // Destruction order matters: the watcher must die before its borrowed fd.
    std::unique_ptr<fcitx::EventSourceIO> resultEvent_;
};

class CandidateTranslatorFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new CandidateTranslatorAddon(manager->instance());
    }
};

} // namespace

bool candidate_translator::cpp_self_test() {
    fcitx::Text display("candidate");
    display.append("  ");
    display.append("translation", fcitx::TextFormatFlag::Italic);
    return display.toString() == "candidate  translation";
}

bool candidate_translator::cpp_commit_rewrite_test() {
    const std::unordered_map<std::string, std::string> translations{
        {"汉字", "Chinese character"}, {"相同", "相同"}, {"空", ""}};
    std::string text = "汉字";
    if (!rewriteCommit(translations, text) || text != "Chinese character") {
        return false;
    }
    // Untranslated, identical, and empty results keep the typed text.
    for (const std::string unchanged : {"未翻译", "相同", "空", ""}) {
        text = unchanged;
        if (rewriteCommit(translations, text) || text != unchanged) {
            return false;
        }
    }
    return true;
}

bool candidate_translator::cpp_sentence_extract_test() {
    const std::string mixed = "hello 我用Linux写代码。";
    // Every character forms one run while the budget allows it.
    if (extractSentence(mixed, 17, 120) != mixed) {
        return false;
    }
    // Over budget, only the tail closest to the cursor survives.
    if (extractSentence(mixed, 17, 3) != "代码。") {
        return false;
    }
    // A line break terminates the run.
    const std::string lines = "first line\n第二行";
    if (extractSentence(lines, 14, 120) != "第二行") {
        return false;
    }
    // Nothing before the cursor yields no request.
    if (!extractSentence(mixed, 0, 120).empty() ||
        !extractSentence("", 0, 120).empty()) {
        return false;
    }
    // A cursor past the end is clamped instead of read out of bounds.
    return extractSentence(mixed, 999, 120) == mixed;
}

bool candidate_translator::cpp_candidate_validation_test() {
    // Use the same byte-slice adapter as the addon. Malformed text must reach
    // Rust and be rejected without throwing during a CXX string conversion.
    const std::string han = "\xe6\xb1\x89";
    if (!prepareCandidate(1, han + "\xff").source.empty() ||
        !prepareCandidate(2, "\xff" + han).source.empty()) {
        return false;
    }
    // A slice carries its full length, including bytes after an embedded NUL.
    const auto valid = han + std::string("\0abc", 4);
    auto candidate = prepareCandidate(7, valid);
    return candidate.index == 7 && std::string(candidate.source) == valid &&
           prepareCandidate(8, valid + "\xff").source.empty();
}

rust::Vec<candidate_translator::TranslationResult>
candidate_translator::cpp_wait_for_results(const Translator &translator) {
    fcitx::EventLoop loop;
    rust::Vec<TranslationResult> results;
    auto watcher = loop.addIOEvent(
        translator.result_fd(), fcitx::IOEventFlag::In,
        [&](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
            results = translator.take_results();
            loop.exit();
            return true;
        });
    auto timeout = loop.addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 5'000'000, 0,
        [&](fcitx::EventSourceTime *, std::uint64_t) {
            loop.exit();
            return false;
        });
    if (!watcher || !timeout) {
        return results;
    }
    loop.exec();
    return results;
}

fcitx::AddonFactory *candidate_translator::addon_factory() {
    static CandidateTranslatorFactory factory;
    return &factory;
}
