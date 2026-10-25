// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import androidx.recyclerview.widget.RecyclerView
import java.text.DateFormat
import java.util.Date
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.ItemSaveTileBinding
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameIconUtils

/** One save tile; [state] is set on the source side only (how this app's copy compares). */
data class SaveTile(
    val entry: SaveEntry,
    val game: Game?,
    val state: DestinationState? = null,
    val selected: Boolean = false,
    val copied: Boolean = false
)

class SaveTileAdapter(
    private val onClick: (SaveTile) -> Unit = {}
) : RecyclerView.Adapter<SaveTileAdapter.Holder>() {
    var tiles: List<SaveTile> = emptyList()
        private set

    fun submit(newTiles: List<SaveTile>) {
        tiles = newTiles
        notifyDataSetChanged()
    }

    override fun getItemCount() = tiles.size

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
        Holder(ItemSaveTileBinding.inflate(LayoutInflater.from(parent.context), parent, false))

    override fun onBindViewHolder(holder: Holder, position: Int) = holder.bind(tiles[position])

    inner class Holder(private val binding: ItemSaveTileBinding) : RecyclerView.ViewHolder(binding.root) {
        fun bind(tile: SaveTile) {
            val context = binding.root.context
            val name = tile.game?.title?.takeIf { it.isNotBlank() } ?: tile.entry.titleId
            binding.title.text = name
            binding.date.text = context.getString(
                R.string.save_transfer_updated,
                DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date(tile.entry.lastModified))
            )
            if (tile.game != null) {
                binding.icon.scaleType = ImageView.ScaleType.CENTER_CROP
                binding.icon.setPadding(0, 0, 0, 0)
                GameIconUtils.loadGameIcon(tile.game, binding.icon)
            } else {
                val pad = context.resources.displayMetrics.density * 24
                binding.icon.scaleType = ImageView.ScaleType.FIT_CENTER
                binding.icon.setPadding(pad.toInt(), pad.toInt(), pad.toInt(), pad.toInt())
                binding.icon.setImageResource(R.drawable.ic_save)
            }
            binding.check.visibility = if (tile.selected || tile.copied) View.VISIBLE else View.GONE
            val label = when {
                tile.copied -> R.string.save_transfer_state_copied
                tile.state == DestinationState.NONE -> R.string.save_transfer_state_new
                tile.state == DestinationState.OLDER -> R.string.save_transfer_state_newer
                tile.state == DestinationState.SAME -> R.string.save_transfer_state_same
                tile.state == DestinationState.NEWER -> R.string.save_transfer_state_older
                else -> 0
            }
            binding.state.visibility = if (label == 0) View.GONE else View.VISIBLE
            if (label != 0) binding.state.setText(label)
            binding.root.alpha = if (tile.copied) 0.7f else 1f
            binding.root.contentDescription = listOfNotNull(
                name,
                binding.date.text,
                if (label != 0) context.getString(label) else null
            ).joinToString(", ")
            binding.root.setOnClickListener { onClick(tile) }
        }
    }
}
