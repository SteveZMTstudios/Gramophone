/*
 *     Copyright (C) 2024 nift4
 *
 *     Gramophone is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     Gramophone is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

package org.akanework.gramophone.logic.utils.exoplayer

import android.content.Context
import android.os.Bundle
import androidx.media3.common.C
import androidx.media3.common.DeviceInfo
import androidx.media3.common.ForwardingSimpleBasePlayer
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackParameters
import androidx.media3.common.Player
import androidx.media3.common.util.Log
import androidx.media3.exoplayer.ExoPlayer
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import org.akanework.gramophone.BuildConfig
import org.akanework.gramophone.R
import org.akanework.gramophone.logic.MultiQueueObject
import org.akanework.gramophone.logic.QueueBoard
import org.akanework.gramophone.logic.parseQueueTitle
import org.akanework.gramophone.logic.utils.CircularShuffleOrder
import org.akanework.gramophone.logic.utils.Flags
import org.akanework.gramophone.logic.utils.SemanticLyrics
import org.json.JSONObject
import androidx.preference.PreferenceManager
import org.akanework.gramophone.logic.getBooleanStrict
import uk.akane.libphonograph.items.EXTRA_HD_ARTWORK_URI
import uk.akane.libphonograph.items.hdArtworkUri
import java.util.Objects


/**
 * If player in STATE_ENDED is resumed, state will be STATE_READY, on play button press it will
 * update to STATE_ENDED and only then media3 will wrap around playlist for us. This is a workaround
 * to restore STATE_ENDED as well and fake it for media3 until it indeed wraps around playlist.
 */
class EndedWorkaroundPlayer(
    val context: Context,
    exoPlayer: ExoPlayer,
    private val getLyric: () -> SemanticLyrics?,
    val queueBoard: QueueBoard,
    private val getNotificationLyric: () -> CharSequence? = { null }
) : ForwardingSimpleBasePlayer(exoPlayer),
    Player.Listener {

    companion object {
        private const val TAG = "EndedWorkaroundPlayer"

    }

    private val remoteDeviceInfo = DeviceInfo.Builder(DeviceInfo.PLAYBACK_TYPE_REMOTE).build()

    init {
        player.addListener(this)
    }

    val exoPlayer
        get() = player as ExoPlayer

    var nextShuffleOrder:
            ((firstIndex: Int, mediaItemCount: Int, EndedWorkaroundPlayer) -> CircularShuffleOrder)? =
        null
    var currentQueueId: Long? = null
    var currentTitle: String? = null
    var currentIsPinned = false
    var currentIsOriginal = false
    private var isEnded = false
        set(value) {
            if (BuildConfig.DEBUG) {
                Log.d(TAG, "isEnded set to $value (was $field)")
            }
            field = value
        }

    override fun onPositionDiscontinuity(
        oldPosition: Player.PositionInfo,
        newPosition: Player.PositionInfo,
        reason: Int
    ) {
        if (reason == DISCONTINUITY_REASON_SEEK) {
            isEnded = false
        }
        super.onPositionDiscontinuity(oldPosition, newPosition, reason)
    }

    fun updateLyricNow() {
        invalidateState()
    }

    fun invalidatePlayerState() {
        invalidateState()
    }

    override fun getState(): State {
        var state = super.state
        state = applyHdArtwork(state)
        state = applyNotificationLyrics(state)
        state = applyOplusLyrics(state)
        return applyEndedOrDeviceState(state)
    }

    private fun applyHdArtwork(state: State): State {
        if (state.currentMetadata.artworkUri != null && state.currentMetadata.hdArtworkUri != null) {
            val updatedMetadata = state.currentMetadata.buildUpon()
                .setArtworkUri(state.currentMetadata.hdArtworkUri)
                .setExtras(Bundle(state.currentMetadata.extras!!).apply {
                    remove(EXTRA_HD_ARTWORK_URI)
                })
                .build()
            return state.buildUpon()
                .setPlaylist(state.timeline, state.currentTracks, updatedMetadata)
                .build()
        }
        return state
    }

    private fun applyNotificationLyrics(state: State): State {
        val notifLyric = getNotificationLyric()
        if (!notifLyric.isNullOrBlank()) {
            val origTitle = state.currentMetadata.title?.toString() ?: ""
            val origArtist = state.currentMetadata.artist?.toString() ?: ""
            val subtitle = if (origArtist.isNotBlank() && origTitle.isNotBlank()) {
                "$origArtist - $origTitle"
            } else {
                origArtist.ifBlank { origTitle }
            }
            val metadataWithLyric = state.currentMetadata.buildUpon()
                .setTitle(notifLyric)
                .setArtist(subtitle)
                .setDisplayTitle(notifLyric)
                .setSubtitle(subtitle)
                .build()
            return state.buildUpon()
                .setPlaylist(state.timeline, state.currentTracks, metadataWithLyric)
                .build()
        }
        return state
    }

    private fun applyOplusLyrics(state: State): State {
        if (context.packageName == "com.tencent.qqmusic") {
            val lyric = getLyric()
            if (lyric is SemanticLyrics.SyncedLyrics) {
                val extras = (state.currentMetadata.extras?.let { Bundle(it) } ?: Bundle()).apply {
                    putString("lyricInfo", buildOplusLyricJson(state, lyric))
                }
                val metadata = state.currentMetadata.buildUpon().setExtras(extras).build()
                return state.buildUpon()
                    .setPlaylist(state.timeline, state.currentTracks, metadata)
                    .build()
            }
        }
        return state
    }

    private fun buildOplusLyricJson(state: State, lyric: SemanticLyrics.SyncedLyrics): String {
        return JSONObject().apply {
            put("songName", state.currentMetadata.title)
            put("artist", state.currentMetadata.artist)
            put(
                "songId",
                state.playlist.getOrNull(state.currentMediaItemIndex)?.mediaItem?.mediaId.toString() +
                        Objects.toIdentityString(lyric)
            )
            put(
                "lyric",
                lyric.text.joinToString("\n") {
                    val s = it.start.toLong() / 1000
                    "[%02d:%02d.%02d]".format(s / 60, s % 60, (it.start.toLong() % 1000) / 10) + it.text
                }
            )
        }.toString()
    }

    private fun applyEndedOrDeviceState(state: State): State {
        if (isEnded) {
            if (state.playerError != null) {
                isEnded = false
                return state
            }
            return state.buildUpon().setPlaybackState(STATE_ENDED).setIsLoading(false).build()
        }
        if (player.currentTimeline.isEmpty) {
            return state.buildUpon().setDeviceInfo(remoteDeviceInfo).build()
        }
        return state
    }


    /**
     * =================
     * Multiqueue Support
     * =================
     */


    /**
     * Multiqueue aware variant of [Player.setMediaItems]
     *
     * This function can be used to in place of [Player.setMediaItems]
     */
    fun setMediaItems(
        mediaItems: List<MediaItem>,
        startIndex: Int,
        startPositionMs: Long,
        title: String,
        pinned: Boolean,
        original: Boolean,
        newShuffleOrder: CircularShuffleOrder.Persistent?,
        ended: Boolean,
        repeatMode: Int?,
        shuffleModeEnabled: Boolean?,
        playbackParameters: PlaybackParameters?,
    ) {
        cloneQueue(generateQueueId(), title, pinned, original)
        if (nextShuffleOrder != null)
            throw IllegalStateException("shuffleFactory was found orphaned")
        if (repeatMode != null) super.handleSetRepeatMode(repeatMode)
        if (shuffleModeEnabled != null) super.handleSetShuffleModeEnabled(shuffleModeEnabled)
        if (playbackParameters != null) super.handleSetPlaybackParameters(playbackParameters)
        nextShuffleOrder = newShuffleOrder?.toFactory()
        super.handleSetMediaItems(mediaItems, startIndex, startPositionMs)
        if (nextShuffleOrder != null)
            throw IllegalStateException("shuffleFactory was not consumed during set")
        isEnded = ended
    }

    /**
     * Variant [setMediaItems]. Load media items into the player without interrupting playback, if possible.
     *
     * This function can be used to in place of [Player.setMediaItems]
     */
    fun setMediaItemsSeamlessly(
        mediaItems: List<MediaItem>,
        startIndex: Int,
        title: String,
        pinned: Boolean,
        original: Boolean,
        repeatMode: Int?,
        shuffleModeEnabled: Boolean?,
        playbackParameters: PlaybackParameters?,
    ) {
        if (startIndex == C.INDEX_UNSET)
            throw IllegalArgumentException("Can't seamlessly set playlist with default position")
        if (nextShuffleOrder != null)
            throw IllegalStateException("shuffleFactory was found orphaned")
        if (currentMediaItem?.mediaId == mediaItems[startIndex].mediaId) {
            replaceMediaItemsSeamlessly(
                mediaItems, startIndex, title, pinned, original,
                repeatMode, shuffleModeEnabled, playbackParameters
            )
        } else {
            setMediaItems(
                mediaItems, startIndex, C.TIME_UNSET, title, pinned,
                original, null, false, repeatMode, shuffleModeEnabled,
                playbackParameters
            )
        }
    }

    private fun replaceMediaItemsSeamlessly(
        mediaItems: List<MediaItem>,
        startIndex: Int,
        title: String,
        pinned: Boolean,
        original: Boolean,
        repeatMode: Int?,
        shuffleModeEnabled: Boolean?,
        playbackParameters: PlaybackParameters?,
    ) {
        val index = currentMediaItemIndex
        val isLast = mediaItemCount - index == 1
        cloneQueue(generateQueueId(), title, pinned, original)
        if (repeatMode != null) super.handleSetRepeatMode(repeatMode)
        if (shuffleModeEnabled != null) super.handleSetShuffleModeEnabled(shuffleModeEnabled)
        if (playbackParameters != null) super.handleSetPlaybackParameters(playbackParameters)
        if (index == 0)
            super.handleAddMediaItems(0, mediaItems.subList(0, startIndex))
        else
            super.handleReplaceMediaItems(0, index, mediaItems.subList(0, startIndex))
        super.handleReplaceMediaItems(startIndex, startIndex, listOf(mediaItems[startIndex]))
        if (isLast) {
            if (mediaItems.size > startIndex + 1)
                super.handleAddMediaItems(Int.MAX_VALUE, mediaItems.subList(startIndex + 1, mediaItems.size))
        } else {
            val tail = if (mediaItems.size > startIndex + 1) mediaItems.subList(startIndex + 1, mediaItems.size) else emptyList()
            super.handleReplaceMediaItems(startIndex + 1, Int.MAX_VALUE, tail)
        }
    }

    /**
     * Saves the active queue to QueueBoard, and updates the next queue's metadata in [EndedWorkaroundPlayer].
     *
     * Saving queues will be refused when either: Queue is empty, or the next queue is the same active queue.
     */
    fun cloneQueue(nextQueueId: Long, nextTitle: String, nextIsPinned: Boolean, nextIsOriginal: Boolean) {
        if (nextTitle == currentTitle && (currentIsOriginal && nextIsOriginal)) return // active queue update, not for queueboard
        if (currentQueueId == null && !exoPlayer.currentTimeline.isEmpty)
            throw IllegalStateException("have media items but current title is null, logic bug")
        else if (currentQueueId != null && Flags.MQ_PREVIEW) {
            saveActiveQueueToBoard()
        }
        currentQueueId = nextQueueId
        currentTitle = nextTitle
        currentIsPinned = nextIsPinned
        currentIsOriginal = nextIsOriginal
    }

    private fun saveActiveQueueToBoard() {
        queueBoard.addQueue(
            currentQueueId!!,
            currentTitle!!,
            ArrayList<MediaItem>(exoPlayer.mediaItemCount).apply {
                for (i in 0..<exoPlayer.mediaItemCount) {
                    add(exoPlayer.getMediaItemAt(i))
                }
            },
            exoPlayer.currentMediaItemIndex,
            exoPlayer.currentPosition,
            currentIsPinned,
            currentIsOriginal,
            repeatMode,
            if (shuffleModeEnabled) {
                CircularShuffleOrder.Persistent(exoPlayer.shuffleOrder as CircularShuffleOrder)
            } else null,
            exoPlayer.playbackState == STATE_ENDED,
        )
    }

    override fun handleAddMediaItems(index: Int, mediaItems: List<MediaItem>): ListenableFuture<*> {
        currentIsOriginal = false
        return super.handleAddMediaItems(index, mediaItems)
    }

    override fun handleMoveMediaItems(
        fromIndex: Int,
        toIndex: Int,
        newIndex: Int
    ): ListenableFuture<*> {
        currentIsOriginal = false
        return super.handleMoveMediaItems(fromIndex, toIndex, newIndex)
    }

    override fun handleReplaceMediaItems(
        fromIndex: Int,
        toIndex: Int,
        mediaItems: List<MediaItem>
    ): ListenableFuture<*> {
        currentIsOriginal = false
        return super.handleReplaceMediaItems(fromIndex, toIndex, mediaItems)
    }

    override fun handleRemoveMediaItems(fromIndex: Int, toIndex: Int): ListenableFuture<*> {
        currentIsOriginal = false
        if (fromIndex == 0 && toIndex >= mediaItemCount) { // clearMediaItems() -> delete queue
            currentTitle = null
            currentQueueId = null
        }
        return super.handleRemoveMediaItems(fromIndex, toIndex)
    }

    override fun handleSetMediaItems(
        mediaItems: List<MediaItem>,
        startIndex: Int,
        startPositionMs: Long
    ): ListenableFuture<*> {
        val idWithTitle = parseQueueTitle(mediaItems.first())
        val title = idWithTitle.second
        val list = if (title != null) mediaItems.toMutableList().apply {
            this[0] = this[0].buildUpon()
                .setMediaId(idWithTitle.first)
                .build()
        } else mediaItems
        val qt = title ?: context.getString(R.string.unknown_playlist)
        setMediaItems(
            list, startIndex, startPositionMs, qt, false,
            true, null, false, null,
            null, null
        )
        return Futures.immediateVoidFuture()
    }

    override fun handleSeek(
        mediaItemIndex: Int,
        positionMs: Long,
        seekCommand: Int
    ): ListenableFuture<*> {
        val prefs = PreferenceManager.getDefaultSharedPreferences(context)
        val alwaysSkipPrevious = prefs.getBooleanStrict("always_skip_previous", false)
        if (seekCommand == Player.COMMAND_SEEK_TO_PREVIOUS && alwaysSkipPrevious) {
            player.seekToPreviousMediaItem()
            return Futures.immediateVoidFuture()
        }
        return super.handleSeek(mediaItemIndex, positionMs, seekCommand)
    }



    /**
     * =================
     * Helpers
     * =================
     */

    /**
     * Get the next available queue ID
     */
    fun generateQueueId(): Long {
        return (queueBoard.masterQueues.map { it.id } + (currentQueueId ?: 0)).max() + 1
    }

    /**
     * Retrieve a snapshot of the active queue in the player.
     */
    fun getActiveQueue(): MultiQueueObject {
        return MultiQueueObject(
            id = currentQueueId!!,
            index = 0,
            title = currentTitle ?: context.getString(R.string.unknown_playlist),
            expiry = if (currentIsPinned) null else 0L,
            queue = ArrayList(),
            startIndex = currentMediaItemIndex,
            startPositionMs = C.TIME_UNSET,
            repeatMode = repeatMode,
            shuffleOrder = null,
            ended = playbackState == STATE_ENDED,
            isOriginal = currentIsOriginal,
        )
    }
}