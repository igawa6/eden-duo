// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.app.Dialog
import android.content.Context
import android.content.DialogInterface
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.BaseAdapter
import android.widget.EditText
import android.widget.ImageView
import android.widget.ListView
import android.widget.RadioButton
import android.widget.TextView
import androidx.core.widget.doAfterTextChanged
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting

/**
 * The No Companion "App" picker: a search field over the apps, each row a radio button, the
 * app's launcher icon and its label. A pick calls [DialogInterface.OnClickListener.onClick] with
 * the choice's index in the setting, as the plain single-choice list does.
 */
object CompanionAppPicker {
    fun create(
        context: Context,
        item: StringSingleChoiceSetting,
        listener: DialogInterface.OnClickListener
    ): Dialog {
        val view = LayoutInflater.from(context).inflate(R.layout.dialog_companion_app_picker, null)
        val list = view.findViewById<ListView>(R.id.list)
        val adapter = Adapter(context, item)
        list.adapter = adapter
        val dialog = MaterialAlertDialogBuilder(context)
            .setTitle(item.title)
            .setView(view)
            .create()
        // The search field takes the first focus (a controller's D-pad): no keyboard until it
        // is clicked.
        dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN)
        list.setOnItemClickListener { _, _, position, _ ->
            listener.onClick(dialog, adapter.getItem(position))
        }
        view.findViewById<EditText>(R.id.search).doAfterTextChanged {
            adapter.filter(it?.toString().orEmpty())
        }
        adapter.rows.indexOf(item.selectedValueIndex).takeIf { it >= 0 }?.let(list::setSelection)
        return dialog
    }

    private class Adapter(
        private val context: Context,
        private val item: StringSingleChoiceSetting
    ) : BaseAdapter() {
        private val selected = item.selectedValueIndex

        /** Indexes into the setting's choices that match the search. */
        var rows: List<Int> = item.choices.indices.toList()
            private set

        /** Keeps every app whose label or package contains [query]. */
        fun filter(query: String) {
            val q = query.trim()
            rows = item.choices.indices.filter { i ->
                q.isEmpty() ||
                    item.choices[i].contains(q, ignoreCase = true) ||
                    item.values[i].contains(q, ignoreCase = true)
            }
            notifyDataSetChanged()
        }

        override fun getCount(): Int = rows.size

        override fun getItem(position: Int): Int = rows[position]

        override fun getItemId(position: Int): Long = rows[position].toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val row = convertView ?: LayoutInflater.from(context)
                .inflate(R.layout.list_item_companion_app, parent, false)
            val index = rows[position]
            row.findViewById<RadioButton>(R.id.radio).isChecked = index == selected
            row.findViewById<TextView>(R.id.label).text = item.choices[index]
            val icon = CompanionApp.pickerIcon(context, item.values[index])
            row.findViewById<ImageView>(R.id.icon).apply {
                setImageDrawable(icon)
                // A row without an icon (an app no longer installed) keeps the space, so every
                // label starts at the same place.
                visibility = if (icon != null) View.VISIBLE else View.INVISIBLE
            }
            return row
        }
    }
}
