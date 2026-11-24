package com.velmux.mobile

import android.app.ActivityManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
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
 * The service is also the platform resource host (spec §21): while it
 * lives, it feeds the core scheduler thermal/battery/memory state through
 * the same bridge plumbing the RN module uses (VelmuxJni
 * .nativeUpdateResources), so concurrency follows real device conditions.
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
    notifications = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
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
    startResourceMonitoring()
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
    stopResourceMonitoring()
    EngineHolder.removeListener(this)
    EngineHolder.serviceRunning = false
    super.onDestroy()
  }

  override fun onBind(intent: Intent?): IBinder? = null

  // -- Platform resource feed (spec §21) -----------------------------------------
  //
  // The core scheduler consumes thermal/battery/memory state to steer
  // concurrency. This service is the Android-side host of that feed: while
  // it exists (work is active), thermal-status and battery listeners plus
  // a slow periodic refresh push updates through the bridge, in EXACTLY
  // the JSON shape velmux_update_resources parses. Pushes are debounced:
  // only real state changes leave the process, at most every few seconds.
  // Every listener is unregistered in onDestroy.

  private val resourceHandler = Handler(Looper.getMainLooper())
  private var resourceMonitoring = false
  private var lastResourcePushMs = 0L
  private var lastResourcePayload: String? = null

  /** Thermal status -> normalized pressure (0 = cool, 1 = emergency). */
  private fun thermalPressureOf(status: Int): Double = when (status) {
    PowerManager.THERMAL_STATUS_NONE, PowerManager.THERMAL_STATUS_LIGHT -> 0.0
    PowerManager.THERMAL_STATUS_MODERATE -> 0.5
    PowerManager.THERMAL_STATUS_SEVERE -> 0.75
    PowerManager.THERMAL_STATUS_CRITICAL -> 0.9
    PowerManager.THERMAL_STATUS_EMERGENCY,
    PowerManager.THERMAL_STATUS_SHUTDOWN -> 1.0
    else -> 0.0
  }

  /**
   * True while the device draws battery power (not plugged in). Read from
   * the ACTION_BATTERY_CHANGED sticky broadcast, which also carries the
   * charge level (the engine's resource model consumes the power source;
   * the level rides along on the same broadcast for future use).
   */
  private fun onBatteryPower(): Boolean {
    val sticky = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
      ?: return false
    return sticky.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) == 0
  }

  /** Available RAM in MB per ActivityManager (0 when unavailable). */
  private fun availableMemoryMb(): Long {
    val activityManager =
      getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
    val info = ActivityManager.MemoryInfo()
    activityManager.getMemoryInfo(info)
    return info.availMem / (1024L * 1024L)
  }

  /**
   * Pushes the current snapshot through the bridge. Debounced: identical
   * state never re-sends, and no two sends are closer than the throttle
   * window unless [force] (memory-pressure events must land immediately).
   */
  private fun pushResources(force: Boolean = false) {
    val handle = EngineHolder.engineHandleOrNull() ?: return
    val now = SystemClock.elapsedRealtime()
    if (!force && now - lastResourcePushMs < RESOURCE_MIN_INTERVAL_MS) {
      return
    }
    val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
    val payload = JSONObject().apply {
      put("cpuCores", Runtime.getRuntime().availableProcessors())
      put("memoryAvailableMb", availableMemoryMb())
      put("thermalPressure", thermalPressureOf(powerManager.currentThermalStatus))
      put("onBattery", onBatteryPower())
    }
    if (payload.toString() == lastResourcePayload) {
      lastResourcePushMs = now
      return
    }
    if (VelmuxJni.nativeUpdateResources(handle, payload.toString())) {
      lastResourcePayload = payload.toString()
      lastResourcePushMs = now
    }
  }

  private val batteryReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context?, intent: Intent?) {
      // Charging state flips matter to the scheduler; level-only changes
      // are covered by the periodic refresh.
      pushResources()
    }
  }

  private val thermalListener =
    PowerManager.OnThermalStatusChangedListener { pushResources() }

  private val resourceRefresh = object : Runnable {
    override fun run() {
      if (!resourceMonitoring) {
        return
      }
      pushResources()
      resourceHandler.postDelayed(this, RESOURCE_REFRESH_MS)
    }
  }

  private fun startResourceMonitoring() {
    if (resourceMonitoring) {
      return
    }
    resourceMonitoring = true
    // ACTION_BATTERY_CHANGED is a protected system sticky broadcast: it is
    // delivered immediately on registration (first push) and on every
    // level/charging change afterwards.
    registerReceiver(batteryReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
    powerManager.addThermalStatusListener(thermalListener)
    resourceHandler.post(resourceRefresh)
  }

  private fun stopResourceMonitoring() {
    if (!resourceMonitoring) {
      return
    }
    resourceMonitoring = false
    resourceHandler.removeCallbacks(resourceRefresh)
    try {
      unregisterReceiver(batteryReceiver)
    } catch (error: IllegalArgumentException) {
      // Already unregistered; nothing to clean.
    }
    val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
    try {
      powerManager.removeThermalStatusListener(thermalListener)
    } catch (error: IllegalArgumentException) {
      // Listener never attached; nothing to clean.
    }
  }

  override fun onTrimMemory(level: Int) {
    super.onTrimMemory(level)
    if (resourceMonitoring) {
      // Memory just tightened: the scheduler should know now, not at the
      // next periodic refresh.
      pushResources(force = true)
    }
  }

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
    // Resource feed cadence: a slow periodic refresh (the listeners cover
    // real changes) and a hard floor between pushes.
    const val RESOURCE_REFRESH_MS = 5_000L
    const val RESOURCE_MIN_INTERVAL_MS = 2_000L
  }
}
