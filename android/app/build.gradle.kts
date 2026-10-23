plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "app.airplaysender"
    compileSdk = 34
    defaultConfig {
        applicationId = "app.airplaysender"
        minSdk = 29 // AudioPlaybackCapture needs API 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.0.1"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    testOptions { unitTests.isReturnDefaultValues = true }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("org.bouncycastle:bcprov-jdk18on:1.78.1") // SRP-6a, HKDF, ChaCha20-Poly1305
    testImplementation("junit:junit:4.13.2")
}
