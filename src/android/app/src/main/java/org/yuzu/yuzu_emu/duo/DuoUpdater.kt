// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.app.PendingIntent
import android.content.ActivityNotFoundException
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import android.provider.Settings
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.core.content.edit
import androidx.core.net.toUri
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.textview.MaterialTextView
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.yuzu.yuzu_emu.BuildConfig
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.settings.model.AbstractBooleanSetting
import org.yuzu.yuzu_emu.features.settings.model.view.RunnableSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.SwitchSetting
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame

/**
 * Eden Duo: in-app update from Eden Duo's GitHub releases (DuoUpdate holds the rules). At most
 * once a day, after first-time setup and never while a game runs, MainActivity asks for the
 * latest release; when it is newer than this build the user can update (download, check, then
 * the system installer installs it over this app, keeping its data), wait, or skip that version.
 */
object DuoUpdater {
    private const val TAG = "[DuoUpdater]"
    private const val KEY_ENABLED = "duo_update_checks"
    private const val KEY_LAST_CHECK = "duo_update_last_check"
    private const val KEY_SKIPPED = "duo_update_skipped"
    private const val UPDATE_DIR = "duo-update"
    private const val MAX_APK_BYTES = 256L * 1024 * 1024

    /** The published app id; dev builds (.dev) cannot install a release over themselves. */
    private const val RELEASE_PACKAGE = "dev.igawa6.edenduo"

    private val client = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .callTimeout(10, TimeUnit.SECONDS)
        .build()

    // The APK takes longer than 10 s on a slow link; each read still times out after 10 s.
    private val downloadClient = client.newBuilder().callTimeout(0, TimeUnit.SECONDS).build()

    private val busy = AtomicBoolean(false)

    private val prefs get() = SecondScreenPerGame.prefs(YuzuApplication.appContext)

    private val currentVersion: String get() = BuildConfig.VERSION_NAME

    fun isEnabled(): Boolean = prefs.getBoolean(KEY_ENABLED, true)

    /** "Check for Eden Duo updates" in App Settings; stored in Eden Duo's preferences. */
    val setting: AbstractBooleanSetting = object : AbstractBooleanSetting {
        override fun getBoolean(needsGlobal: Boolean): Boolean = isEnabled()
        override fun setBoolean(value: Boolean) = prefs.edit { putBoolean(KEY_ENABLED, value) }
        override val key: String = KEY_ENABLED
        override val isRuntimeModifiable: Boolean = true
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = false
        override var global: Boolean = true
        override val isSaveable: Boolean = true
        override val defaultValue: Boolean = true
        override fun getValueAsString(needsGlobal: Boolean): String = isEnabled().toString()
        override fun reset() = prefs.edit { remove(KEY_ENABLED) }
    }

    /** The App Settings items (SettingsFragmentPresenter): the switch and "Check now". */
    fun addTo(sl: ArrayList<SettingsItem>, activity: FragmentActivity?) {
        sl.add(
            SwitchSetting(
                setting,
                titleId = R.string.duo_update_checks,
                descriptionId = R.string.duo_update_checks_description
            )
        )
        sl.add(
            RunnableSetting(
                titleId = R.string.duo_update_check_now,
                descriptionString = YuzuApplication.appContext.getString(
                    R.string.duo_update_check_now_description,
                    currentVersion
                ),
                isRunnable = !NativeLibrary.isRunning()
            ) { activity?.let { check(it, manual = true) } }
        )
    }

    /** MainActivity start: the daily check, when it is on and due, in a release build. */
    fun checkOnStart(activity: FragmentActivity) {
        if (!isEnabled() || activity.packageName != RELEASE_PACKAGE || NativeLibrary.isRunning()) {
            return
        }
        if (!DuoUpdate.isDue(prefs.getLong(KEY_LAST_CHECK, 0), System.currentTimeMillis())) return
        check(activity, manual = false)
    }

    private sealed class Answer {
        data class Latest(val release: DuoUpdate.Release?) : Answer()
        object RateLimited : Answer()
        data class Failed(val reason: String) : Answer()
    }

    private fun check(activity: FragmentActivity, manual: Boolean) {
        if (!busy.compareAndSet(false, true)) return
        activity.lifecycleScope.launch {
            try {
                val answer = withContext(Dispatchers.IO) {
                    removeOldDownloads(activity.cacheDir)
                    fetchLatest()
                }
                handle(activity, answer, manual)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                log("check failed: ${e.message}")
            } finally {
                busy.set(false)
            }
        }
    }

    private fun fetchLatest(): Answer = try {
        val request = Request.Builder()
            .url(DuoUpdate.LATEST_URL)
            .header("Accept", "application/vnd.github+json")
            .header("User-Agent", "EdenDuo/$currentVersion")
            .build()
        client.newCall(request).execute().use { response ->
            when (response.code) {
                200 -> Answer.Latest(DuoUpdate.parseLatest(response.body?.string() ?: ""))
                // No release yet, or only pre-releases: nothing to offer.
                404 -> Answer.Latest(null)
                403, 429 -> Answer.RateLimited
                else -> Answer.Failed("HTTP ${response.code}")
            }
        }
    } catch (e: IOException) {
        Answer.Failed(e.message ?: e.javaClass.simpleName)
    }

    private fun handle(activity: FragmentActivity, answer: Answer, manual: Boolean) {
        val now = System.currentTimeMillis()
        when (answer) {
            is Answer.RateLimited -> {
                log("GitHub rate limit; next check later")
                if (!manual) prefs.edit { putLong(KEY_LAST_CHECK, now) }
                if (manual) toast(activity, R.string.duo_update_check_busy)
            }
            is Answer.Failed -> {
                log("check failed: ${answer.reason}")
                if (manual) toast(activity, R.string.duo_update_check_failed)
            }
            is Answer.Latest -> {
                val release = answer.release
                val skipped = if (manual) null else prefs.getString(KEY_SKIPPED, null)
                if (!DuoUpdate.shouldOffer(release, currentVersion, skipped) || release == null) {
                    log("latest ${release?.version ?: "none"}, installed $currentVersion: no offer")
                    if (!manual) prefs.edit { putLong(KEY_LAST_CHECK, now) }
                    if (manual) toast(activity, R.string.duo_update_up_to_date)
                    return
                }
                // A game may have started meanwhile, or the screen gone: ask on a later start.
                if (NativeLibrary.isRunning() ||
                    !activity.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
                ) {
                    return
                }
                log("offering ${release.version} (installed $currentVersion)")
                prefs.edit { putLong(KEY_LAST_CHECK, now) }
                showOffer(activity, release)
            }
        }
    }

    private fun showOffer(activity: FragmentActivity, release: DuoUpdate.Release) {
        val notes = DuoUpdate.plainNotes(release.notes)
        val canInstall = activity.packageName == RELEASE_PACKAGE
        MaterialAlertDialogBuilder(activity)
            .setTitle(activity.getString(R.string.duo_update_available, release.version))
            .setMessage(
                notes.ifEmpty {
                    activity.getString(R.string.duo_update_available_no_notes, currentVersion)
                }
            )
            .setPositiveButton(
                if (canInstall) R.string.duo_update_install else R.string.duo_update_view
            ) { _, _ ->
                if (canInstall) download(activity, release) else openPage(activity, release)
            }
            .setNeutralButton(R.string.duo_update_later, null)
            .setNegativeButton(R.string.duo_update_skip) { _, _ ->
                prefs.edit { putString(KEY_SKIPPED, release.version) }
                log("skipping ${release.version}")
            }
            .show()
    }

    private fun openPage(activity: FragmentActivity, release: DuoUpdate.Release) {
        try {
            activity.startActivity(Intent(Intent.ACTION_VIEW, release.pageUrl.toUri()))
        } catch (e: ActivityNotFoundException) {
            log("no browser for ${release.pageUrl}")
        }
    }

    private class Cancelled : IOException("cancelled")

    private fun download(activity: FragmentActivity, release: DuoUpdate.Release) {
        if (!busy.compareAndSet(false, true)) return
        val view = activity.layoutInflater.inflate(R.layout.dialog_download_progress, null)
        val bar = view.findViewById<LinearProgressIndicator>(R.id.download_progress_bar)
        val label = view.findViewById<MaterialTextView>(R.id.download_progress_message)
        val cancelled = AtomicBoolean(false)
        val dialog: AlertDialog = MaterialAlertDialogBuilder(activity)
            .setTitle(activity.getString(R.string.duo_update_downloading, release.version))
            .setView(view)
            .setCancelable(false)
            .setNegativeButton(android.R.string.cancel) { _, _ -> cancelled.set(true) }
            .show()
        label.text = activity.getString(R.string.percent, 0)
        var shown = -1
        activity.lifecycleScope.launch {
            try {
                val apk = withContext(Dispatchers.IO) {
                    downloadApk(activity.cacheDir, release) { copied, total ->
                        ensureActive()
                        if (cancelled.get()) throw Cancelled()
                        val percent = if (total > 0) (copied * 100 / total).toInt() else 0
                        if (percent != shown) {
                            shown = percent
                            activity.runOnUiThread {
                                bar.progress = percent
                                label.text = activity.getString(R.string.percent, percent)
                            }
                        }
                    }.also { verifyPackage(activity, it) }
                }
                dialog.dismiss()
                busy.set(false)
                install(activity, apk)
            } catch (e: Cancelled) {
                log("download of ${release.version} cancelled")
            } catch (e: CancellationException) {
                log("download of ${release.version} stopped: the screen closed")
                throw e
            } catch (e: Exception) {
                dialog.dismiss()
                log("update ${release.version} failed: ${e.message}")
                toast(activity, R.string.duo_update_failed)
            } finally {
                if (dialog.isShowing) dialog.dismiss()
                busy.set(false)
            }
        }
    }

    /** Downloads [release]'s APK into the cache; checks its size and SHA-256 (when given). */
    private fun downloadApk(
        cacheDir: File,
        release: DuoUpdate.Release,
        progress: (copied: Long, total: Long) -> Unit
    ): File {
        if (release.size > MAX_APK_BYTES) throw IOException("APK too large: ${release.size}")
        val dir = File(cacheDir, UPDATE_DIR)
        dir.deleteRecursively()
        if (!dir.mkdirs()) throw IOException("cannot create $dir")
        val apk = File(dir, release.assetName)
        try {
            val request = Request.Builder()
                .url(release.downloadUrl)
                .header("User-Agent", "EdenDuo/$currentVersion")
                .build()
            downloadClient.newCall(request).execute().use { response ->
                if (!response.isSuccessful) throw IOException("HTTP ${response.code}")
                val body = response.body ?: throw IOException("empty download")
                val total = release.size.takeIf { it > 0 } ?: body.contentLength()
                val digest = MessageDigest.getInstance("SHA-256")
                var copied = 0L
                body.byteStream().use { input ->
                    apk.outputStream().use { output ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            progress(copied, total)
                            val read = input.read(buffer)
                            if (read == -1) break
                            copied += read
                            if (copied > MAX_APK_BYTES) throw IOException("APK too large")
                            output.write(buffer, 0, read)
                            digest.update(buffer, 0, read)
                        }
                    }
                }
                progress(copied, total)
                if (release.size > 0 && copied != release.size) {
                    throw IOException("incomplete download: $copied of ${release.size} bytes")
                }
                log("verified size $copied bytes" + if (release.size > 0) "" else " (none given)")
                val actual = digest.digest().joinToString("") { "%02x".format(it) }
                if (release.sha256 == null) {
                    log("no digest given (sha256 $actual)")
                } else if (actual != release.sha256) {
                    throw IOException("SHA-256 mismatch: $actual")
                } else {
                    log("verified sha256 $actual")
                }
            }
            return apk
        } catch (e: Exception) {
            apk.delete()
            throw e
        }
    }

    /** The download must be an APK the PackageManager reads, for this very app id. */
    private fun verifyPackage(context: Context, apk: File) {
        val info = context.packageManager.getPackageArchiveInfo(apk.path, 0)
        if (info == null || info.packageName != context.packageName) {
            apk.delete()
            throw IOException("not an update for ${context.packageName}: ${info?.packageName}")
        }
        log("verified package ${info.packageName} (versionName ${info.versionName})")
    }

    /**
     * Hands the verified [apk] to the system installer through a PackageInstaller session (a
     * plain ACTION_VIEW opens an "Open with" chooser wherever file managers claim APKs). Without
     * the "Install unknown apps" permission the user is sent to that setting first.
     */
    private fun install(activity: FragmentActivity, apk: File) {
        if (activity.packageManager.canRequestPackageInstalls()) {
            commitSession(activity, apk)
            return
        }
        log("asking for the install-unknown-apps permission")
        val registry = activity.activityResultRegistry
        lateinit var launcher: ActivityResultLauncher<Intent>
        launcher = registry.register(
            "duo-update-unknown-sources",
            ActivityResultContracts.StartActivityForResult()
        ) {
            launcher.unregister()
            if (activity.packageManager.canRequestPackageInstalls()) {
                commitSession(activity, apk)
            } else {
                log("install-unknown-apps permission not given")
                apk.delete()
                toast(activity, R.string.duo_update_unknown_sources)
            }
        }
        try {
            launcher.launch(
                Intent(
                    Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    "package:${activity.packageName}".toUri()
                )
            )
        } catch (e: ActivityNotFoundException) {
            launcher.unregister()
            log("no install-unknown-apps settings screen; trying the installer anyway")
            commitSession(activity, apk)
        }
    }

    private fun commitSession(activity: FragmentActivity, apk: File) {
        val context = activity.applicationContext
        activity.lifecycleScope.launch {
            try {
                withContext(Dispatchers.IO) { writeAndCommit(context, apk) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                log("installer could not start: ${e.message}")
                toast(activity, R.string.duo_update_install_not_started)
            } finally {
                // The session holds its own copy once written.
                apk.delete()
            }
        }
    }

    private fun writeAndCommit(context: Context, apk: File) {
        val installer = context.packageManager.packageInstaller
        // An earlier attempt that never finished (the app was closed meanwhile).
        installer.mySessions.forEach { runCatching { installer.abandonSession(it.sessionId) } }
        val params =
            PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        params.setAppPackageName(context.packageName)
        params.setSize(apk.length())
        val sessionId = installer.createSession(params)
        val session = installer.openSession(sessionId)
        try {
            session.openWrite("base.apk", 0, apk.length()).use { output ->
                apk.inputStream().use { it.copyTo(output, 64 * 1024) }
                session.fsync(output)
            }
            val intent = Intent(context, DuoInstallReceiver::class.java)
                .setAction(DuoInstallReceiver.ACTION)
            // Mutable: the installer adds the status extras to this explicit intent.
            val mutable = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                PendingIntent.FLAG_MUTABLE
            } else {
                0
            }
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or mutable
            val pending = PendingIntent.getBroadcast(context, sessionId, intent, flags)
            session.commit(pending.intentSender)
            log("install session $sessionId committed (${apk.length()} bytes)")
        } catch (e: Exception) {
            runCatching { session.abandon() }
            throw e
        } finally {
            session.close()
        }
    }

    /** DuoInstallReceiver: the session's answer. */
    internal fun onInstallStatus(context: Context, intent: Intent) {
        val status =
            intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
        val sessionId = intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1)
        val outcome = DuoUpdate.installOutcome(status, message)
        log("install session $sessionId: status $status ($outcome) ${message ?: ""}".trimEnd())
        val text = when (outcome) {
            DuoUpdate.InstallOutcome.NEEDS_CONFIRMATION -> {
                val confirm = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(Intent.EXTRA_INTENT)
                }
                try {
                    context.startActivity(confirm!!.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                    null
                } catch (e: Exception) {
                    log("installer confirmation could not open: ${e.message}")
                    val installer = context.packageManager.packageInstaller
                    runCatching { installer.abandonSession(sessionId) }
                    R.string.duo_update_install_not_started
                }
            }
            DuoUpdate.InstallOutcome.SUCCESS -> R.string.duo_update_installed
            DuoUpdate.InstallOutcome.CANCELLED -> R.string.duo_update_install_cancelled
            DuoUpdate.InstallOutcome.DOWNGRADE -> R.string.duo_update_install_downgrade
            DuoUpdate.InstallOutcome.CONFLICT -> R.string.duo_update_install_conflict
            DuoUpdate.InstallOutcome.INVALID -> R.string.duo_update_install_invalid
            DuoUpdate.InstallOutcome.INCOMPATIBLE -> R.string.duo_update_install_incompatible
            DuoUpdate.InstallOutcome.STORAGE -> R.string.duo_update_install_storage
            DuoUpdate.InstallOutcome.BLOCKED -> R.string.duo_update_install_blocked
            DuoUpdate.InstallOutcome.FAILED -> R.string.duo_update_install_failed
        }
        if (outcome != DuoUpdate.InstallOutcome.NEEDS_CONFIRMATION) {
            File(context.cacheDir, UPDATE_DIR).deleteRecursively()
        }
        text?.let { Toast.makeText(context, it, Toast.LENGTH_LONG).show() }
    }

    // Leftovers of an update that never reached the installer (the app was closed meanwhile).
    private fun removeOldDownloads(cacheDir: File) {
        val dir = File(cacheDir, UPDATE_DIR)
        val hourAgo = System.currentTimeMillis() - 60 * 60 * 1000
        dir.listFiles()?.filter { it.lastModified() < hourAgo }?.forEach { it.delete() }
    }

    private fun toast(context: Context, message: Int) =
        Toast.makeText(context, message, Toast.LENGTH_SHORT).show()

    private fun log(line: String) = Log.info("$TAG $line")
}

/** Eden Duo: receives the in-app update's PackageInstaller session status (DuoUpdater). */
class DuoInstallReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == ACTION) DuoUpdater.onInstallStatus(context, intent)
    }

    companion object {
        const val ACTION = "dev.igawa6.edenduo.UPDATE_INSTALL_STATUS"
    }
}
