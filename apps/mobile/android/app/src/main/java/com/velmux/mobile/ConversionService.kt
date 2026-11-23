package com.velmux.mobile

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import org.json.JSONObject
import java.util.concurrent.ConcurrentHashMap

/**
 * Foreground host for active conversions (spec §22, M8).
 *
 * Android keeps the process alive while this service is foreground, which
 * keeps the C++ engine's worker threads running when the UI is gone. The
 * service renders normalized engine events as platform notifications:
 *
 *  - one live progress notification per running job (coalesced cadence);
 *  - terminal notifications on completion/failure/cancel;
 *  - a Cancel action that goes straight to the engine — it works without
 *    the React Native UI being initialized (spec §23).
 *
 * The service starts from EngineHolder when work becomes active and stops
 * itself when the engine idles. It owns no media policy.
 */
class ConversionService : Service(), EngineHolder.EventListener {

  private lateinit var notifications: NotificationManager

  // JobId -> last progress fraction (0..1, or -1). The live notification
  // per job reuses the same id for seamless updates.
  private val liveJobs = ConcurrentHashMap<String, Double>()

  override fun onCreate() {
    super.onCreate()
    notifications = getSystemService(Context.NOTIFICATION_SERVICE)
      as NotificationManager
    notifications.createNotificationChannel(
      NotificationChannel(
        CHANNEL_ACTIVE,
        getString(R.string.conversion_channel_active),
        NotificationManager.IMPORTANCE_LOW, // progress: silent, visible
      ).apply { setShowBadge(false) },
    )
    notifications.createNotificationChannel(
      NotificationChannel(
        CHANNEL_RESULTS,
        getString(R.string.conversion_channel_results),
        NotificationManager.IMPORTANCE_DEFAULT, // results: audible, visible
      ),
    )
    EngineHolder.addListener(this)
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    // Always enter the foreground first: every start path (including a
    // notification Cancel action after the service was recreated) must
    // call startForeground within the window or the system kills us.
    promoteForeground()
    // The listener registered in onCreate keeps the event pump alive
    // while the service exists.
    if (intent?.action == ACTION_CANCEL) {
      val jobId = intent.getStringExtra(EXTRA_JOB_ID)
      if (!jobId.isNullOrEmpty()) {
        EngineHolder.cancel(jobId)
      }
      // The service keeps running: the cancel's terminal event decides.
    }
    return START_NOT_STICKY
  }

  /** Enters the foreground state with the initial (indeterminate) notice. */
  private fun promoteForeground() {
    val notification =
      Notification.Builder(this, CHANNEL_ACTIVE)
        .setSmallIcon(android.R.drawable.stat_sys_download)
        .setContentTitle(getString(R.string.conversion_preparing))
        .setOnlyAlertOnce(true)
        .setOngoing(true)
        .setContentIntent(contentIntent())
        .setProgress(0, 0, true)
        .build()
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      startForeground(
        FOREGROUND_ID,
        notification,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC,
      )
    } else {
      startForeground(FOREGROUND_ID, notification)
    }
  }

  override fun onDestroy() {
    EngineHolder.removeListener(this)
    EngineHolder.serviceRunning = false
    super.onDestroy()
  }

  override fun onBind(intent: Intent?): IBinder? = null

  // -- EngineHolder.EventListener --------------------------------------------------

  override fun onJobEvent(event: JSONObject) {
    val jobId = event.optString("jobId")
    val state = event.optString("state")
    val progress = event.optDouble("progress", -1.0)

    when (state) {
      "preparing", "running", "checkpointing", "recovering" -> {
        liveJobs[jobId] = progress
        updateForegroundNotification()
      }
      "completed" -> {
        liveJobs.remove(jobId)
        val size = event.optLong("outputSizeBytes", -1L)
        val text =
          if (size > 0) {
            getString(R.string.conversion_completed_size, humanSize(size))
          } else {
            getString(R.string.conversion_completed)
          }
        notify(jobId.hashCode(), buildResultNotification(
          getString(R.string.conversion_completed_title), text))
        settle()
      }
      "failed" -> {
        liveJobs.remove(jobId)
        notify(jobId.hashCode(), buildResultNotification(
          getString(R.string.conversion_failed_title),
          event.optString("error", getString(R.string.conversion_failed))))
        settle()
      }
      "canceled" -> {
        liveJobs.remove(jobId)
        notify(jobId.hashCode(), buildResultNotification(
          getString(R.string.conversion_canceled_title),
          getString(R.string.conversion_canceled)))
        settle()
      }
      "paused", "interrupted" -> {
        liveJobs.remove(jobId)
        // No notification: the user or the OS paused it; the Queue screen
        // is the surface for those states.
        settle()
      }
      else -> {
        // queued and unknown states do not change the notification set.
      }
    }
  }

  /**
   * Updates the FOREGROUND notification from the live job set, then stops
   * the service when the engine idles.
   */
  private fun settle() {
    updateForegroundNotification()
    if (!EngineHolder.hasActiveWork()) {
      stopForeground(STOP_FOREGROUND_REMOVE)
      stopSelf()
    }
  }

  /**
   * The aggregate live notification: one job -> its progress with a Cancel
   * action; several -> the count with the mean progress. Reuses the
   * foreground id so the platform swaps the notice in place.
   */
  private fun updateForegroundNotification() {
    if (liveJobs.isEmpty()) {
      notifications.cancel(FOREGROUND_ID)
      return
    }
    if (liveJobs.size == 1) {
      val (jobId, progress) = liveJobs.entries.single().let {
        it.key to it.value
      }
      val builder =
        Notification.Builder(this, CHANNEL_ACTIVE)
          .setSmallIcon(android.R.drawable.stat_sys_download)
          .setContentTitle(getString(R.string.conversion_running))
          .setOnlyAlertOnce(true)
          .setOngoing(true)
          .setContentIntent(contentIntent())
          .addAction(
            Notification.Action.Builder(
              null,
              getString(R.string.conversion_action_cancel),
              cancelIntent(jobId),
            ).build(),
          )
      if (progress in 0.0..1.0) {
        builder.setProgress(100, (progress * 100).toInt(), false)
      } else {
        builder.setProgress(0, 0, true)
      }
      notifications.notify(FOREGROUND_ID, builder.build())
      return
    }
    val known = liveJobs.values.filter { it in 0.0..1.0 }
    val mean =
      if (known.isEmpty()) -1.0 else known.average()
    val builder =
      Notification.Builder(this, CHANNEL_ACTIVE)
        .setSmallIcon(android.R.drawable.stat_sys_download)
        .setContentTitle(getString(R.string.conversion_running_many,
                                   liveJobs.size))
        .setOnlyAlertOnce(true)
        .setOngoing(true)
        .setContentIntent(contentIntent())
    if (mean in 0.0..1.0) {
      builder.setProgress(100, (mean * 100).toInt(), false)
    } else {
      builder.setProgress(0, 0, true)
    }
    notifications.notify(FOREGROUND_ID, builder.build())
  }

  private fun notify(id: Int, notification: Notification) {
    notifications.notify(id, notification)
  }

  // -- notifications ------------------------------------------------------------------

  private fun buildResultNotification(title: String, text: String): Notification =
    Notification.Builder(this, CHANNEL_RESULTS)
      .setSmallIcon(android.R.drawable.stat_sys_download_done)
      .setContentTitle(title)
      .setContentText(text)
      .setAutoCancel(true)
      .setContentIntent(contentIntent())
      .build()

  private fun contentIntent(): PendingIntent {
    val launch = packageManager.getLaunchIntentForPackage(packageName)
    val request =
      if (launch != null) {
        Intent(launch).setPackage(packageName)
      } else {
        Intent(this, MainActivity::class.java)
      }
    return PendingIntent.getActivity(
      this,
      0,
      request,
      PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
    )
  }

  private fun cancelIntent(jobId: String): PendingIntent {
    val intent =
      Intent(this, ConversionService::class.java)
        .setAction(ACTION_CANCEL)
        .putExtra(EXTRA_JOB_ID, jobId)
    return PendingIntent.getService(
      this,
      jobId.hashCode(),
      intent,
      PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
    )
  }

  private fun humanSize(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val kb = bytes / 1024.0
    if (kb < 1024) return String.format("%.0f KB", kb)
    val mb = kb / 1024.0
    if (mb < 1024) return String.format("%.1f MB", mb)
    return String.format("%.2f GB", mb / 1024.0)
  }

  private companion object {
    const val CHANNEL_ACTIVE = "velmux.conversions.active"
    const val CHANNEL_RESULTS = "velmux.conversions.results"
    const val FOREGROUND_ID = 42
    const val ACTION_CANCEL = "com.velmux.mobile.action.CANCEL_JOB"
    const val EXTRA_JOB_ID = "jobId"
  }
}
