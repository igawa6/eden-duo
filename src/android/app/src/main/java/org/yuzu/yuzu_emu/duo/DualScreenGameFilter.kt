// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.SharedPreferences
import android.view.Menu
import android.view.MenuItem
import android.widget.TextView
import androidx.core.content.edit
import androidx.lifecycle.LifecycleOwner
import com.google.android.material.button.MaterialButton
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.GameAdapter
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.utils.ViewUtils.setVisible
import org.yuzu.yuzu_emu.utils.collect

/**
 * Eden Duo: the games list's dual-screen part -- the "dual-screen games only" filter button, the
 * companion badges' data, the "not in your library" section's games and switch, and the
 * empty-list notice that knows about the filter.
 *
 * [refilter] re-runs the fragment's filter and search; [adapter] is the fragment's GameAdapter.
 */
class DualScreenGameFilter(
    private val preferences: SharedPreferences,
    private val gamesViewModel: () -> GamesViewModel,
    private val adapter: () -> GameAdapter,
    private val refilter: () -> Unit
) {
    private companion object {
        const val PREF_DUAL_SCREEN_ONLY = "GamesDualScreenOnly"
        const val PREF_SHOW_UNOWNED = "GamesShowUnownedDualScreen"
    }

    private var dualScreenOnly = preferences.getBoolean(PREF_DUAL_SCREEN_ONLY, false)
    private var showUnowned = preferences.getBoolean(PREF_SHOW_UNOWNED, true)

    /** [games] reduced to the titles with a companion while the filter is on. */
    fun filter(games: List<Game>): List<Game> =
        if (dualScreenOnly) {
            games.filter { it.programIdHex in gamesViewModel().dualScreen.dualScreenTitleIds.value }
        } else {
            games
        }

    /**
     * The companion games the library does not have, for the section after the games: none
     * while the switch is off, [searchTerm] is not empty, the list is not in its full order
     * ([fullList] false: recently played/added) or a first library scan is still running.
     */
    fun unowned(searchTerm: String, fullList: Boolean): List<CompanionCatalog.Entry> {
        val viewModel = gamesViewModel()
        val library = viewModel.games.value
        if (!CompanionCatalog.sectionVisible(showUnowned, searchTerm, fullList) ||
            (library.isEmpty() && viewModel.isReloading.value)
        ) {
            return emptyList()
        }
        return CompanionCatalog.unowned(
            viewModel.dualScreen.dualScreenTitleIds.value,
            library.map { it.programIdHex }
        )
    }

    /** Adds the "show dual-screen games not in your library" switch to the filter menu. */
    fun addMenuItem(menu: Menu) {
        menu.add(
            Menu.NONE,
            R.id.companion_unowned_toggle,
            Menu.CATEGORY_SECONDARY,
            R.string.companion_unowned_setting
        ).apply {
            isCheckable = true
            isChecked = showUnowned
        }
    }

    /** Handles a filter-menu click; false when [item] is not the switch. */
    fun onMenuItem(item: MenuItem): Boolean {
        if (item.itemId != R.id.companion_unowned_toggle) return false
        showUnowned = !showUnowned
        preferences.edit { putBoolean(PREF_SHOW_UNOWNED, showUnowned) }
        item.isChecked = showUnowned
        refilter()
        return true
    }

    /** Feeds the companion badges and refilters when the supported titles change. */
    fun collect(owner: LifecycleOwner) {
        gamesViewModel().dualScreen.dualScreenInstallStates.collect(owner) {
            adapter().dualScreenBadges.setInstallStates(it)
        }
        gamesViewModel().dualScreen.dualScreenTitleIds.collect(owner) {
            adapter().dualScreenBadges.setTitleIds(it)
            refilter()
        }
    }

    fun bindButton(button: MaterialButton) {
        updateButton(button)
        button.setOnClickListener {
            dualScreenOnly = !dualScreenOnly
            preferences.edit { putBoolean(PREF_DUAL_SCREEN_ONLY, dualScreenOnly) }
            updateButton(button)
            refilter()
        }
    }

    /**
     * The list's empty notice: no games at all, none with a companion, or none matching. Hidden
     * while the "not in your library" section shows ([sectionShown]), which it would cover.
     */
    fun updateNotice(notice: TextView, games: List<Game>, sectionShown: Boolean = false) {
        notice.setText(
            when {
                gamesViewModel().games.value.isEmpty() -> R.string.empty_gamelist
                dualScreenOnly -> R.string.dual_screen_filter_empty
                else -> R.string.game_filter_empty
            }
        )
        notice.setVisible(
            visible = games.isEmpty() && !sectionShown && !gamesViewModel().isReloading.value,
            gone = false
        )
    }

    private fun updateButton(button: MaterialButton) {
        button.apply {
            isChecked = dualScreenOnly
            contentDescription = context.getString(
                if (dualScreenOnly) R.string.dual_screen_filter_show_all else R.string.dual_screen_filter
            )
        }
    }
}
