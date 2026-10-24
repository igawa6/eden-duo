// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import kotlin.coroutines.cancellation.CancellationException

/** Eden Duo: what a ProgressDialogFragment task ends with when it throws. */
class DuoTaskFailure(val error: Exception)

/** Eden Duo: runs a ProgressDialogFragment task; an exception becomes a [DuoTaskFailure]. */
suspend fun runGuardedTask(onError: (Exception) -> Unit, task: suspend () -> Any): Any =
    try {
        task()
    } catch (e: CancellationException) {
        throw e
    } catch (e: Throwable) {
        // An Error (out of memory during a large copy) must end the task too, not the app.
        val error = e as? Exception ?: RuntimeException(e)
        onError(error)
        DuoTaskFailure(error)
    }
