// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.features.settings.ui.viewholder

import android.view.View
import org.yuzu.yuzu_emu.databinding.ListItemSettingBinding
import org.yuzu.yuzu_emu.features.settings.model.view.IntSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.SingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.ui.SettingsAdapter
import org.yuzu.yuzu_emu.utils.CompanionApp
import org.yuzu.yuzu_emu.utils.ViewUtils.setVisible

class SingleChoiceViewHolder(val binding: ListItemSettingBinding, adapter: SettingsAdapter) :
    SettingViewHolder(binding.root, adapter) {
    private lateinit var setting: SettingsItem

    override fun bind(item: SettingsItem) {
        setting = item
        binding.textSettingName.text = setting.title
        binding.textSettingDescription.setVisible(item.description.isNotEmpty())
        binding.textSettingDescription.text = item.description

        binding.textSettingValue.setVisible(true)
        when (item) {
            is SingleChoiceSetting -> {
                val resMgr = binding.textSettingValue.context.resources
                val values = resMgr.getIntArray(item.valuesId)
                for (i in values.indices) {
                    if (values[i] == item.getSelectedValue()) {
                        binding.textSettingValue.text = resMgr.getStringArray(item.choicesId)[i]
                        break
                    }
                }
            }

            is StringSingleChoiceSetting -> {
                // Eden Duo: the No Companion app picker shows the app's name, not its component.
                binding.textSettingValue.text =
                    CompanionApp.rowLabel(item) ?: item.getSelectedValue()
            }

            is IntSingleChoiceSetting -> {
                // Eden Duo fix: getSelectedValue() is the setting's stored *value* (e.g. -1 for
                // Automatic), not its position in choices -- getChoiceAt() needs the latter.
                // Every pre-existing IntSingleChoiceSetting row in this app happens to use an
                // identity values array (values[i] == i), which hid this: for those,
                // selectedValueIndex and getSelectedValue() are the same number. Eden Duo's Mode
                // row (DualScreenMode, values -1/0/1/2) is the first one where they differ, which
                // is what made the summary line lag one entry behind the real selection.
                binding.textSettingValue.text = item.getChoiceAt(item.selectedValueIndex)
            }
        }
        if (binding.textSettingValue.text.isEmpty()) {
            binding.textSettingValue.setVisible(false)
        }

        binding.buttonClear.setVisible(setting.clearable)
        binding.buttonClear.setOnClickListener {
            adapter.onClearClick(setting, bindingAdapterPosition)
        }

        setStyle(setting.isEditable, binding)
    }

    override fun onClick(clicked: View) {
        if (!setting.isEditable) {
            return
        }

        when (setting) {
            is SingleChoiceSetting -> adapter.onSingleChoiceClick(
                setting as SingleChoiceSetting,
                bindingAdapterPosition
            )

            is StringSingleChoiceSetting -> {
                adapter.onStringSingleChoiceClick(
                    setting as StringSingleChoiceSetting,
                    bindingAdapterPosition
                )
            }

            is IntSingleChoiceSetting -> {
                adapter.onIntSingleChoiceClick(
                    setting as IntSingleChoiceSetting,
                    bindingAdapterPosition
                )
            }
        }
    }

    override fun onLongClick(clicked: View): Boolean {
        if (setting.isEditable) {
            return adapter.onLongClick(setting, bindingAdapterPosition)
        }
        return false
    }
}
