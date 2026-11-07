package org.vita3k.emulator.data

import android.content.Context
import androidx.annotation.StringRes
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat
import org.vita3k.emulator.R

data class UiLanguageOption(
    val tag: String,
    @StringRes val labelRes: Int
)

object UiLanguages {
    private const val PREFS_NAME = "vita3k_frontend"
    private const val PREF_UI_LANGUAGE = "ui_language"

    val options: List<UiLanguageOption> = listOf(
        UiLanguageOption("", R.string.ui_language_system_default),
        UiLanguageOption("zh-CN", R.string.ui_language_chinese_simplified),
        UiLanguageOption("en", R.string.ui_language_english)
    )

    fun currentTag(context: Context): String =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_UI_LANGUAGE, "") ?: ""

    fun applyStored(context: Context) {
        apply(currentTag(context))
    }

    fun applyAndPersist(context: Context, tag: String) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_UI_LANGUAGE, tag)
            .apply()
        apply(tag)
    }

    fun apply(tag: String) {
        val locales = if (tag.isBlank()) {
            LocaleListCompat.getEmptyLocaleList()
        } else {
            LocaleListCompat.forLanguageTags(tag)
        }
        AppCompatDelegate.setApplicationLocales(locales)
    }
}
