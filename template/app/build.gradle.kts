plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

fun prop(name: String): String? = (project.findProperty(name) as String?)?.takeIf { it.isNotBlank() }

val gameApplicationId = prop("rom2apk.applicationId") ?: "com.rom2apk.player"
val gameAbis = (prop("rom2apk.abis") ?: "arm64-v8a").split(",").map { it.trim() }
val keystorePath = prop("rom2apk.storeFile")

android {
    namespace = "com.rom2apk.player"
    compileSdk = 35

    defaultConfig {
        applicationId = gameApplicationId
        minSdk = 26
        targetSdk = 35
        versionCode = (prop("rom2apk.versionCode") ?: "1").toInt()
        versionName = prop("rom2apk.versionName") ?: "1.0"
        ndk { abiFilters.addAll(gameAbis) }
    }

    signingConfigs {
        if (keystorePath != null) {
            create("rom2apk") {
                storeFile = file(keystorePath)
                storePassword = prop("rom2apk.storePassword")
                keyAlias = prop("rom2apk.keyAlias")
                keyPassword = prop("rom2apk.keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (keystorePath != null) {
                signingConfigs.getByName("rom2apk")
            } else {
                signingConfigs.getByName("debug")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs { useLegacyPackaging = true }
    }

    lint {
        checkReleaseBuilds = false
    }
}

dependencies {
    implementation(project(":libretrodroid"))
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
}
