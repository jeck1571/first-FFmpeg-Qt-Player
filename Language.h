#pragma once

#include <QString>

// UI language selection. The interface is written in English in the source and
// translated through a small built-in dictionary, so switching languages does
// not require restarting the player or rebuilding the widgets.
enum class UiLanguage {
    English,
    Chinese
};

namespace UiLanguageSupport {

// Returns the Chinese text for an English source string, or the source itself
// when no translation exists.
QString dictionaryText(const QString& english);

UiLanguage fromCode(const QString& code);
QString code(UiLanguage language);
UiLanguage systemDefault();

// Installs or removes the translators. A QEvent::LanguageChange follows, so
// already visible widgets can re-apply their texts.
void setCurrent(UiLanguage language);
UiLanguage current();

} // namespace UiLanguageSupport
