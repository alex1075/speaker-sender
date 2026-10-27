package app.speakersender

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow

data class Speaker(val name: String, val host: String, val port: Int, val txt: Map<String, String>)

/** Browses `_raop._tcp`. Instance names look like `204EF66480F3@Speaker1 speaker`. Untested scaffold. */
class AirPlayDiscovery(context: Context) {
    private val nsd = context.getSystemService(Context.NSD_SERVICE) as NsdManager

    @Suppress("DEPRECATION") // resolveService is deprecated on API 34+; migrate to registerServiceInfoCallback
    fun speakers(): Flow<Speaker> = callbackFlow {
        val listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) { close() }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onServiceLost(info: NsdServiceInfo) {}
            override fun onServiceFound(info: NsdServiceInfo) {
                nsd.resolveService(info, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(i: NsdServiceInfo, errorCode: Int) {}
                    override fun onServiceResolved(i: NsdServiceInfo) {
                        val txt = i.attributes.mapValues { (_, v) -> v?.toString(Charsets.UTF_8) ?: "" }
                        trySend(Speaker(i.serviceName.substringAfter('@'), i.host.hostAddress ?: return, i.port, txt))
                    }
                })
            }
        }
        nsd.discoverServices("_raop._tcp.", NsdManager.PROTOCOL_DNS_SD, listener)
        awaitClose { nsd.stopServiceDiscovery(listener) }
    }
}
