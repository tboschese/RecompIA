pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "rom2apk-player"
include(":app")
// Patched checkout prepared by the rom2apk CLI (see template/libretrodroid.patch).
include(":libretrodroid")
