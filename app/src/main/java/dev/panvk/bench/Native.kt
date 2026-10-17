package dev.panvk.bench

import android.view.Surface

object Native {
    init {
        System.loadLibrary("panvkbench")
    }

    external fun runBenchmark(
        driverPath: String,
        dependencyDirectory: String,
        bundledLibraryDirectory: String,
        surface: Surface,
        testMode: Int,
        durationSeconds: Int,
        objects: Int,
        logPath: String
    ): String

    external fun startPreview(
        driverPath: String,
        dependencyDirectory: String,
        bundledLibraryDirectory: String,
        surface: Surface,
        testMode: Int,
        objects: Int,
        logPath: String
    ): Boolean

    external fun stopPreview()

    external fun requestStop()

    external fun setDebugLogging(enabled: Boolean)

    external fun setSceneRotation(degrees: Float)
}
