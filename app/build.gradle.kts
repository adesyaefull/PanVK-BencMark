import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

val mesaRoot = rootDir.parentFile.parentFile
val localProperties = Properties().apply {
    val propertiesFile = File(rootDir, "local.properties")
    if (propertiesFile.isFile) propertiesFile.inputStream().use { load(it) }
}
val ndkRoot = File(
    localProperties.getProperty(
        "ndk.dir",
        File(mesaRoot.parentFile, "android-ndk-r29").absolutePath
    )
)
val drmProp = providers.gradleProperty("panvkDrm").orNull
val panvkDrmFile = drmProp?.let { file(it) }
    ?: File(mesaRoot, "dist/android-g925-v13-csf/libdrm.so")
val cxxProp = providers.gradleProperty("panvkCxx").orNull
val panvkCxxFile = cxxProp?.let { file(it) }
    ?: File(
        ndkRoot,
        "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
    )

val copyPanvkRuntime = tasks.register<Copy>("copyPanvkRuntime") {
    inputs.files(panvkDrmFile, panvkCxxFile)
    from(panvkDrmFile) { rename { "libdrm.so" } }
    from(panvkCxxFile) { rename { "libc++.so" } }
    into(layout.buildDirectory.dir("generated/panvkJni/arm64-v8a"))
    doFirst {
        if (!panvkDrmFile.isFile) {
            throw GradleException(
                "PanVK libdrm.so not found at ${panvkDrmFile.absolutePath}; set -PpanvkDrm=<path>."
            )
        }
        if (!panvkCxxFile.isFile) {
            throw GradleException(
                "Android libc++ runtime not found at ${panvkCxxFile.absolutePath}; set -PpanvkCxx=<path>."
            )
        }
    }
}

tasks.named("preBuild") {
    dependsOn(copyPanvkRuntime)
}

android {
    namespace = "dev.panvk.bench"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "dev.panvk.bench"
        minSdk = 29
        targetSdk = 36
        versionCode = 3
        versionName = "0.3.0"

        ndk {
            abiFilters.add("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += "-DMESA_ROOT=${mesaRoot.absolutePath}"
                cppFlags += listOf("-std=c++17")
            }
        }
    }

    buildTypes {
        debug {
            isDebuggable = true
        }
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    sourceSets.getByName("main") {
        jniLibs.directories.add("build/generated/panvkJni")
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2026.02.01")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
}
