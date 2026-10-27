package app.speakersender

import android.app.Activity
import android.os.Bundle
import android.widget.TextView

/** Placeholder launcher screen so the APK installs and starts. The speaker picker and start/stop come with the sender. */
class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val text = TextView(this).apply {
            text = "speaker-sender\n\nThe AirPlay handshake is not ported to Kotlin yet."
            textSize = 18f
            setPadding(48, 96, 48, 48)
        }
        setContentView(text)
    }
}
