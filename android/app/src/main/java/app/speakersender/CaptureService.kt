package app.speakersender

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.IBinder
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

/**
 * Captures other apps' audio (API 29+) as 44.1 kHz / 16-bit / stereo PCM in 352-frame chunks (what AirPlay sends per packet).
 * Start it with the result of MediaProjectionManager.createScreenCaptureIntent(). Untested scaffold.
 * Apps can opt out of capture; those will deliver silence.
 */
class CaptureService : Service() {
    private var job: Job? = null
    private var projection: MediaProjection? = null

    /** Set by the UI before starting; receives one 1408-byte chunk per call. */
    var onChunk: (ByteArray) -> Unit = {}

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(1, notification())
        val resultCode = intent?.getIntExtra("resultCode", 0) ?: return START_NOT_STICKY
        val data: Intent = intent.getParcelableExtra("data") ?: return START_NOT_STICKY
        val mpm = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = mpm.getMediaProjection(resultCode, data)
        job = CoroutineScope(Dispatchers.IO).launch { capture(projection!!) }
        return START_NOT_STICKY
    }

    @Suppress("MissingPermission") // RECORD_AUDIO is requested by the UI
    private fun capture(mp: MediaProjection) {
        val config = AudioPlaybackCaptureConfiguration.Builder(mp)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()
        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(44100)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()
        val chunkBytes = 352 * 4
        val record = AudioRecord.Builder()
            .setAudioFormat(format)
            .setAudioPlaybackCaptureConfig(config)
            .setBufferSizeInBytes(chunkBytes * 16)
            .build()
        record.startRecording()
        val buf = ByteArray(chunkBytes)
        try {
            while (job?.isActive == true) {
                var off = 0
                while (off < chunkBytes) {
                    val n = record.read(buf, off, chunkBytes - off)
                    if (n <= 0) return
                    off += n
                }
                onChunk(buf.copyOf())
            }
        } finally {
            record.stop(); record.release()
        }
    }

    private fun notification(): Notification {
        val mgr = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        mgr.createNotificationChannel(NotificationChannel("cap", "Audio capture", NotificationManager.IMPORTANCE_LOW))
        return Notification.Builder(this, "cap").setContentTitle("Streaming to AirPlay").setSmallIcon(android.R.drawable.ic_media_play).build()
    }

    override fun onDestroy() {
        job?.cancel(); projection?.stop()
        super.onDestroy()
    }
}
