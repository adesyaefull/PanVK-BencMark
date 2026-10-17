package dev.panvk.bench

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.Surface
import android.view.TextureView
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Surface as ComposeSurface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.security.MessageDigest
import java.security.NoSuchAlgorithmException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.Locale
import java.util.UUID

private data class ImportedDriver(
    val file: File,
    val name: String,
    val sha256: String,
    val companionNames: List<String>
)

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme(
                primary = Color(0xFF57D7CB),
                secondary = Color(0xFF91A9FF),
                background = Color(0xFF080D15),
                surface = Color(0xFF111A26),
                surfaceVariant = Color(0xFF1A2634)
            )) {
                ComposeSurface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    BenchmarkScreen()
                }
            }
        }
    }
}

@Composable
private fun BenchmarkScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var surface by remember { mutableStateOf<Surface?>(null) }
    var imported by remember { mutableStateOf<ImportedDriver?>(null) }
    var useImported by remember { mutableStateOf(false) }
    var duration by remember { mutableStateOf(60f) }
    var objectCount by remember { mutableStateOf(256f) }
    var running by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("Ready — choose a driver and start a run.") }
    var result by remember { mutableStateOf("") }
    var logText by remember { mutableStateOf("") }
    var vulkanInfoText by remember { mutableStateOf("") }
    var reportJson by remember { mutableStateOf("") }
    var fps by remember { mutableStateOf("--") }
    var phase by remember { mutableStateOf("IDLE") }
    var elapsedSeconds by remember { mutableStateOf(0f) }
    var liveObjects by remember { mutableStateOf(0) }
    var liveFrames by remember { mutableStateOf(0L) }
    var presentIntervalMs by remember { mutableStateOf("--") }
    var decoderThroughput by remember { mutableStateOf("--") }
    var displayRefreshRate by remember { mutableStateOf("--") }
    var testMode by remember { mutableStateOf(0) }
    var sceneRotation by remember { mutableStateOf(0f) }
    var reportTestName by remember { mutableStateOf("") }
    var scoreLine by remember { mutableStateOf("") }
    var selectedTab by remember { mutableStateOf(0) }
    var infoCategory by remember { mutableStateOf(0) }
    var debugLogging by remember {
        mutableStateOf(
            context.getSharedPreferences("benchmark", Context.MODE_PRIVATE)
                .getBoolean("debug_logging", false)
        )
    }
    val onSceneDrag = rememberUpdatedState<(Float) -> Unit> { delta ->
        sceneRotation = (sceneRotation + delta * 0.65f) % 360f
        Native.setSceneRotation(sceneRotation)
    }
    val nativeLibraries = remember(context) {
        File(context.applicationInfo.nativeLibraryDir).list()?.toSet().orEmpty()
    }

    LaunchedEffect(Unit) {
        Native.setDebugLogging(debugLogging)
        try {
            val saved = withContext(Dispatchers.IO) { loadImportedDriver(context) }
            imported = saved
            useImported = saved != null
        } catch (error: IOException) {
            status = "Could not read imported driver: ${error.message ?: "I/O error"}"
        } catch (error: NoSuchAlgorithmException) {
            status = "Could not verify imported driver: SHA-256 unavailable"
        }
    }

    val importPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenMultipleDocuments()
    ) { uris ->
        if (uris.isNotEmpty()) {
            scope.launch {
                status = "Importing driver files privately…"
                try {
                    val newDriver = importDriverBundle(context, uris)
                    imported = newDriver
                    useImported = true
                    status = "Imported ${newDriver.name} · SHA-256 ${newDriver.sha256.take(12)}…"
                } catch (error: IOException) {
                    status = "Import failed: ${error.message ?: "I/O error"}"
                } catch (error: SecurityException) {
                    status = "Import failed: ${error.message ?: "Permission denied"}"
                } catch (error: IllegalArgumentException) {
                    status = "Import failed: ${error.message ?: "Invalid driver file"}"
                } catch (error: IllegalStateException) {
                    status = "Import failed: ${error.message ?: "Could not store driver"}"
                } catch (error: NoSuchAlgorithmException) {
                    status = "Import failed: SHA-256 unavailable"
                }
            }
        }
    }

    val exportPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("application/json")
    ) { uri ->
        if (uri != null) {
            scope.launch(Dispatchers.IO) {
                try {
                    context.contentResolver.openOutputStream(uri, "wt")?.use {
                        it.write(reportJson.toByteArray(Charsets.UTF_8))
                    } ?: error("Could not open the selected destination")
                    withContext(Dispatchers.Main) { status = "Report exported." }
                } catch (error: IOException) {
                    withContext(Dispatchers.Main) {
                        status = "Export failed: ${error.message ?: "I/O error"}"
                    }
                } catch (error: SecurityException) {
                    withContext(Dispatchers.Main) {
                        status = "Export failed: ${error.message ?: "Permission denied"}"
                    }
                } catch (error: IllegalStateException) {
                    withContext(Dispatchers.Main) {
                        status = "Export failed: ${error.message ?: "Invalid destination"}"
                    }
                }
            }
        }
    }

    val logExportPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("text/plain")
    ) { uri ->
        if (uri != null) {
            val textLog = logText
            scope.launch(Dispatchers.IO) {
                try {
                    context.contentResolver.openOutputStream(uri, "wt")?.use {
                        it.write(textLog.toByteArray(Charsets.UTF_8))
                    } ?: error("Could not open the selected destination")
                    withContext(Dispatchers.Main) { status = "Vulkan log exported." }
                } catch (error: IOException) {
                    withContext(Dispatchers.Main) {
                        status = "Log export failed: ${error.message ?: "I/O error"}"
                    }
                } catch (error: SecurityException) {
                    withContext(Dispatchers.Main) {
                        status = "Log export failed: ${error.message ?: "Permission denied"}"
                    }
                } catch (error: IllegalStateException) {
                    withContext(Dispatchers.Main) {
                        status = "Log export failed: ${error.message ?: "Invalid destination"}"
                    }
                }
            }
        }
    }

    val selectedDriver = if (useImported) imported else null
    val selectedDriverPath = selectedDriver?.file?.absolutePath ?: "libvulkan.so"

    LaunchedEffect(surface, running, selectedDriverPath, testMode, objectCount, debugLogging) {
        val target = surface
        if (!running && target?.isValid == true) {
            vulkanInfoText = ""
            val previewLog = File(context.filesDir, "preview-current.log")
            previewLog.delete()
            val started = withContext(Dispatchers.IO) {
                Native.setDebugLogging(debugLogging)
                Native.startPreview(
                    selectedDriverPath,
                    if (selectedDriver != null) {
                        selectedDriver.file.parentFile?.absolutePath
                            ?: File(context.filesDir, "drivers").absolutePath
                    } else {
                        ""
                    },
                    context.applicationInfo.nativeLibraryDir,
                    target,
                    testMode,
                    objectCount.toInt(),
                    previewLog.absolutePath
                )
            }
            if (!started)
                status = "Could not start Vulkan preview."
            else {
                var previewSnapshot = ""
                var attempts = 0
                while (attempts < 40 &&
                    !previewSnapshot.contains("PREVIEW_READY") &&
                    !previewSnapshot.contains("PREVIEW_FAILED")
                ) {
                    delay(250)
                    previewSnapshot = withContext(Dispatchers.IO) {
                        previewLog.takeIf { it.exists() }?.readText().orEmpty()
                    }
                    attempts++
                }
                vulkanInfoText = previewSnapshot
            }
        }
    }

    LaunchedEffect(surface, running) {
        while (running && surface?.isValid == true) {
            delay(750)
            val currentLog = File(context.filesDir, "benchmark-current.log")
            val log = withContext(Dispatchers.IO) {
                currentLog.takeIf { it.exists() }?.readText().orEmpty()
            }
            logText = log.takeLast(120_000)
            val live = log.lineSequence().lastOrNull { it.startsWith("LIVE ") }
            if (live != null) {
                phase = Regex("""phase=([^ ]+)""").find(live)?.groupValues?.get(1) ?: phase
                fps = Regex("""fps=([0-9.]+)""").find(live)?.groupValues?.get(1) ?: fps
                elapsedSeconds = Regex("""render_time_s=([0-9.]+)""")
                    .find(live)?.groupValues?.get(1)?.toFloatOrNull() ?: elapsedSeconds
                presentIntervalMs = Regex("""present_call_interval_ms=([0-9.]+)""")
                    .find(live)?.groupValues?.get(1) ?: presentIntervalMs
                liveObjects = Regex("""objects=(\d+)""")
                    .find(live)?.groupValues?.get(1)?.toIntOrNull() ?: liveObjects
                liveFrames = Regex("""frames=(\d+)""")
                    .find(live)?.groupValues?.get(1)?.toLongOrNull() ?: liveFrames
                decoderThroughput = if (testMode == 6) {
                    Regex("""BC2=([0-9.]+).*BC3=([0-9.]+).*BC4=([0-9.]+).*BC4_SNORM=([0-9.]+).*BC5=([0-9.]+).*BC5_SNORM=([0-9.]+).*BC6H_UFLOAT=([0-9.]+).*BC6H_SFLOAT=([0-9.]+).*BC7_UNORM=([0-9.]+).*BC7_SRGB=([0-9.]+)""")
                        .find(live)?.groupValues?.drop(1)?.joinToString(" / ")
                        ?: decoderThroughput
                } else {
                    Regex("""bc1_mpix_s=([0-9.]+)""")
                        .find(live)?.groupValues?.get(1) ?: decoderThroughput
                }
            }
            scoreLine = log.lineSequence().lastOrNull { it.startsWith("SUMMARY ") }.orEmpty()
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState())
            .padding(horizontal = 18.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        Column {
            Column(Modifier.padding(top = 22.dp, bottom = 2.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    androidx.compose.foundation.Image(
                        painter = painterResource(R.drawable.app_logo),
                        contentDescription = "BenchMark app logo",
                        modifier = Modifier.height(58.dp).padding(end = 12.dp)
                    )
                    Column {
                        Text(
                            "BENCHMARK",
                            color = MaterialTheme.colorScheme.primary,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            letterSpacing = 2.sp
                        )
                        Text("GPU Field Test", fontSize = 28.sp, fontWeight = FontWeight.Bold)
                        Text(
                            "Live Vulkan scene · frame pacing · sustained load",
                            color = Color(0xFFA9B8C9),
                            fontSize = 13.sp
                        )
                    }
                }
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            if (selectedTab == 0) {
                Button(
                    onClick = { selectedTab = 0 },
                    modifier = Modifier.weight(1f)
                ) { Text("BENCHMARK") }
                OutlinedButton(
                    onClick = { selectedTab = 1 },
                    modifier = Modifier.weight(1f)
                ) { Text("VULKAN INFO") }
            } else {
                OutlinedButton(
                    onClick = { selectedTab = 0 },
                    modifier = Modifier.weight(1f)
                ) { Text("BENCHMARK") }
                Button(
                    onClick = { selectedTab = 1 },
                    modifier = Modifier.weight(1f)
                ) { Text("VULKAN INFO") }
            }
        }

        if (selectedTab == 0) {
        Column {
            Card(
                shape = RoundedCornerShape(18.dp),
                colors = CardDefaults.cardColors(containerColor = Color(0xFF111A26))
            ) {
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text("LIVE TELEMETRY", fontWeight = FontWeight.Bold, color = Color(0xFF9EB2C6))
                        if (running) {
                            OutlinedButton(
                                onClick = { Native.requestStop() },
                                contentPadding = ButtonDefaults.TextButtonContentPadding
                            ) { Text("STOP RUN") }
                        }
                    }
                    if (running) {
                        LinearProgressIndicator(
                            progress = { (elapsedSeconds / duration).coerceIn(0f, 1f) },
                            modifier = Modifier.fillMaxWidth()
                        )
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(26.dp)) {
                        Metric("PRESENT FPS", fps)
                        Metric("PHASE", phase)
                        Metric("RESULT", result.ifEmpty { "—" })
                    }
                    if (running) {
                        Text(
                            "Shader time: ${"%.2f".format(elapsedSeconds)} s · " +
                                "last present-call interval: $presentIntervalMs ms",
                            color = Color(0xFFA9B8C9),
                            fontSize = 11.sp
                        )
                        Text(
                            "FPS counts successful vkQueuePresent calls over wall-clock time. " +
                                "Animation time is sampled into that frame from the same monotonic clock; " +
                                "panel scanout timing is not exposed by every ICD.",
                            color = Color(0xFF8292A4),
                            fontSize = 10.sp,
                            lineHeight = 13.sp
                        )
                    }
                    if (running) {
                        val workloadUnit = when (testMode) {
                            5 -> "tess factor"
                            6 -> "BC blocks/format"
                            3 -> "dolphins"
                            4 -> "triangle"
                            7 -> "depth layers"
                            8 -> "blend layers"
                            9 -> "fragment layers"
                            10 -> "triangles"
                            11 -> "transform instances"
                            12 -> "discard layers"
                            13 -> "blend layers"
                            14 -> "geometry triangles"
                            15 -> "triangles / single draw"
                            else -> "instances"
                        }
                        Text(
                            "${elapsedSeconds.toInt()} / ${duration.toInt()} sec  ·  $liveObjects $workloadUnit  ·  $liveFrames frames",
                            color = Color(0xFFA9B8C9),
                            fontSize = 12.sp
                        )
                    }
                    if ((testMode == 1 || testMode == 6) && decoderThroughput != "--") {
                        Text(
                            if (testMode == 6) {
                                "BC2 / BC3 / BC4U / BC4S / BC5U / BC5S / BC6U / BC6S / BC7 / BC7sRGB MPix/s: $decoderThroughput"
                            } else {
                                "BC1 CPU decode: $decoderThroughput MPix/s"
                            },
                            color = Color(0xFF45D6CA),
                            fontSize = 12.sp,
                            fontWeight = FontWeight.SemiBold
                        )
                    }
                    Text(status, color = MaterialTheme.colorScheme.primary, fontSize = 13.sp)
                    if (scoreLine.isNotEmpty()) {
                        Text(
                            scoreLine.removePrefix("SUMMARY "),
                            color = Color(0xFFCEE2E6),
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp,
                            lineHeight = 15.sp
                        )
                    }
                    Text(
                        "Device: ${Build.MANUFACTURER} ${Build.MODEL} · Android ${Build.VERSION.RELEASE} · display $displayRefreshRate",
                        color = Color(0xFFA9B8C9),
                        fontSize = 12.sp
                    )
                }
            }
        }

        Column {
            Card(
                shape = RoundedCornerShape(20.dp),
                colors = CardDefaults.cardColors(containerColor = Color(0xFF111A26))
            ) {
                Column(Modifier.fillMaxWidth().padding(12.dp)) {
                    Text(
                        "VULKAN BENCHMARK OUTPUT",
                        fontWeight = FontWeight.Bold,
                        color = Color(0xFF9EB2C6),
                        fontSize = 12.sp,
                        letterSpacing = 1.5.sp
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        if (running) "LIVE RENDER · $liveObjects OBJECTS · $liveFrames FRAMES"
                        else "Vulkan preview ready · FPS telemetry runs only during a benchmark.",
                        color = if (running) MaterialTheme.colorScheme.primary else Color(0xFFA9B8C9),
                        fontSize = 11.sp,
                        fontWeight = FontWeight.SemiBold
                    )
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.End
                    ) {
                        OutlinedButton(
                            onClick = {
                                sceneRotation = 0f
                                Native.setSceneRotation(0f)
                            },
                            contentPadding = ButtonDefaults.TextButtonContentPadding
                        ) { Text("RESET VIEW") }
                    }
                    Spacer(Modifier.height(6.dp))
                    AndroidView(
                        modifier = Modifier.fillMaxWidth().height(220.dp).pointerInput(Unit) {
                            detectDragGestures { change, dragAmount ->
                                change.consume()
                                onSceneDrag.value(dragAmount.x)
                            }
                        },
                        factory = { viewContext ->
                            TextureView(viewContext).also { view ->
                                view.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                                    override fun onSurfaceTextureAvailable(
                                        texture: android.graphics.SurfaceTexture,
                                        width: Int,
                                        height: Int
                                    ) {
                                        val output = Surface(texture)
                                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                                            val maxRefreshRate = view.display?.supportedModes
                                                ?.maxOfOrNull { it.refreshRate }
                                            if (maxRefreshRate != null) {
                                                output.setFrameRate(
                                                    maxRefreshRate,
                                                    Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
                                                )
                                                displayRefreshRate =
                                                    "${maxRefreshRate.toInt()} Hz requested"
                                            }
                                        }
                                        surface = output
                                    }

                                    override fun onSurfaceTextureSizeChanged(
                                        texture: android.graphics.SurfaceTexture,
                                        width: Int,
                                        height: Int
                                    ) = Unit

                                    override fun onSurfaceTextureDestroyed(
                                        texture: android.graphics.SurfaceTexture
                                    ): Boolean {
                                        val oldSurface = surface
                                        surface = null
                                        scope.launch(Dispatchers.IO) {
                                            Native.stopPreview()
                                            oldSurface?.release()
                                        }
                                        return true
                                    }

                                    override fun onSurfaceTextureUpdated(
                                        texture: android.graphics.SurfaceTexture
                                    ) = Unit
                                }
                            }
                        }
                    )
                }
            }
        }

        Column {
            Card(
                shape = RoundedCornerShape(18.dp),
                colors = CardDefaults.cardColors(containerColor = Color(0xFF111A26))
            ) {
                Column(
                    Modifier.fillMaxWidth().padding(14.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text("BENCH SUITE", fontWeight = FontWeight.Bold, color = Color(0xFF9EB2C6))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SuiteButton("Cube", testMode == 0, !running, Modifier.weight(1f)) {
                            testMode = 0
                            objectCount = 256f
                        }
                        SuiteButton("BC1 Emulation", testMode == 1, !running, Modifier.weight(1f)) {
                            testMode = 1
                            objectCount = 256f
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SuiteButton("Draw Stress 1K–2K", testMode == 2, !running, Modifier.weight(1f)) {
                            testMode = 2
                            objectCount = 1024f
                        }
                        SuiteButton("Dolphin 3D", testMode == 3, !running, Modifier.weight(1f)) {
                            testMode = 3
                            objectCount = 3f
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SuiteButton("Triangle", testMode == 4, !running, Modifier.weight(1f)) {
                            testMode = 4
                            objectCount = 1f
                        }
                        SuiteButton("Tess + Shadow", testMode == 5, !running, Modifier.weight(1f)) {
                            testMode = 5
                            objectCount = 12f
                        }
                    }
                    SuiteButton("BC2–BC7 CPU Emulation", testMode == 6, !running, Modifier.fillMaxWidth()) {
                        testMode = 6
                        objectCount = 512f
                    }
                    Text(
                        "DRIVER DEVELOPMENT · RECOMMENDED",
                        color = MaterialTheme.colorScheme.primary,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 1.sp
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SuiteButton("Depth / Occlusion", testMode == 7, !running, Modifier.weight(1f)) {
                            testMode = 7
                            objectCount = 128f
                        }
                        SuiteButton("Alpha Blend", testMode == 8, !running, Modifier.weight(1f)) {
                            testMode = 8
                            objectCount = 64f
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SuiteButton("Fragment Shader", testMode == 9, !running, Modifier.weight(1f)) {
                            testMode = 9
                            objectCount = 32f
                        }
                        SuiteButton("Triangle Raster", testMode == 10, !running, Modifier.weight(1f)) {
                            testMode = 10
                            objectCount = 1024f
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SuiteButton("Vertex Transform", testMode == 11, !running, Modifier.weight(1f)) {
                            testMode = 11
                            objectCount = 512f
                        }
                        SuiteButton("Fragment Discard", testMode == 12, !running, Modifier.weight(1f)) {
                            testMode = 12
                            objectCount = 24f
                        }
                    }
                    SuiteButton("Blend Overdraw", testMode == 13, !running, Modifier.fillMaxWidth()) {
                        testMode = 13
                        objectCount = 64f
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SuiteButton("Geometry Test", testMode == 14, !running, Modifier.weight(1f)) {
                            testMode = 14
                            objectCount = 512f
                        }
                        SuiteButton("Large Draw Test", testMode == 15, !running, Modifier.weight(1f)) {
                            testMode = 15
                            objectCount = 16384f
                        }
                    }
                    Text(
                        when (testMode) {
                            1 -> "Animated BC1 selector mosaic is rendered live while the CPU decodes blocks; this is visually distinct from the cube suite."
                            2 -> "Animated field of 1,024–2,048 rotating triangle instances stresses dense draws; it does not render cubes."
                            3 -> "Low-poly dolphin meshes and tail animation are rendered directly by the selected Vulkan ICD."
                            4 -> "A rotating triangle rendered directly to the Vulkan surface."
                            5 -> "A Vulkan tessellation pipeline animates a subdivided surface and a projected shadow. Requires tessellationShader support."
                            6 -> "Ten animated BC-format selector panels are rendered live instead of cubes while the CPU decodes BC2, BC3, BC4/5 unsigned/signed, BC6H unsigned/signed, and BC7 linear/sRGB blocks."
                            7 -> "Depth/occlusion profile: rotating triangles at different depths overlap so depth test/write and early rejection are exercised."
                            8 -> "Alpha-blend profile: animated translucent layers use Vulkan src-alpha blending with depth writes disabled."
                            9 -> "Fragment-shader profile: animated full-screen overlapping quads run a repeated trigonometric shader workload with depth disabled."
                            10 -> "Triangle-raster profile: 1,024–4,096 animated micro-triangles exercise primitive setup and raster coverage."
                            11 -> "Vertex-transform profile: dense animated meshes run multiple transform and trigonometric operations per vertex."
                            12 -> "Fragment-discard profile: animated layered quads exercise fragment coverage, discard, and depth-disabled overdraw."
                            13 -> "Blend-overdraw profile: translucent full-screen layers stress source-alpha blending and color attachment traffic."
                            14 -> "Geometry profile: a dense animated triangular field stresses vertex processing, primitive assembly, and rasterization."
                            15 -> "Large-draw profile: submits tens of thousands of triangles in one Vulkan draw call to stress large vertex workloads."
                            else -> "Rotating instanced cubes rendered directly to the Vulkan surface."
                        },
                        color = Color(0xFFA9B8C9),
                        fontSize = 11.sp,
                        lineHeight = 15.sp
                    )
                }
            }
        }

        Column {
            Card(
                shape = RoundedCornerShape(18.dp),
                colors = CardDefaults.cardColors(containerColor = Color(0xFF111A26))
            ) {
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    SettingSlider(
                        title = "Run duration",
                        value = duration,
                        valueText = "${duration.toInt()} sec",
                        range = 20f..180f,
                        steps = 7,
                        enabled = !running
                    ) { duration = it }
                    Spacer(Modifier.height(4.dp))
                    if (testMode != 4) {
                        val objectRange = when (testMode) {
                            2 -> 1024f..2048f
                            3 -> 1f..12f
                            5 -> 2f..32f
                            6 -> 64f..4096f
                            7 -> 32f..512f
                            8 -> 16f..128f
                            9 -> 16f..128f
                            10 -> 1024f..4096f
                            11 -> 256f..2048f
                            12 -> 8f..64f
                            13 -> 16f..128f
                            14 -> 128f..2048f
                            15 -> 4096f..65536f
                            else -> 64f..1024f
                        }
                        val objectSteps = when (testMode) {
                            2 -> 15
                            3 -> 10
                            5 -> 29
                            6 -> 63
                            7 -> 29
                            8 -> 27
                            9 -> 27
                            10 -> 47
                            11 -> 27
                            12 -> 27
                            13 -> 27
                            14 -> 14
                            15 -> 14
                            else -> 14
                        }
                        val objectLabel = when (testMode) {
                            1 -> "Scene cubes"
                            2 -> "Stress draw instances"
                            3 -> "Dolphins"
                            5 -> "Tessellation factor"
                            6 -> "BC blocks per format / frame"
                            7 -> "Depth layers"
                            8 -> "Blended layers"
                            9 -> "Fragment overdraw layers"
                            10 -> "Triangle instances"
                            11 -> "Transform instances"
                            12 -> "Overdraw layers"
                            13 -> "Blend layers"
                            14 -> "Geometry triangles"
                            15 -> "Triangles in one draw"
                            else -> "Animated cubes"
                        }
                        SettingSlider(
                            title = objectLabel,
                            value = objectCount.coerceIn(objectRange.start, objectRange.endInclusive),
                            valueText = objectCount.toInt().coerceIn(
                                objectRange.start.toInt(), objectRange.endInclusive.toInt()
                            ).toString(),
                            range = objectRange,
                            steps = objectSteps,
                            enabled = !running
                        ) { objectCount = it }
                    }
                    Text(
                        if (testMode == 1) "Software BC1 decode throughput is measured alongside the animated selector mosaic."
                        else if (testMode == 2) "Triangle instances rotate and pulse; stress mode sustains at least 1,024 instances through every load phase."
                        else if (testMode == 5) "Shadow geometry is projected by the tessellation-evaluation shader; it is not a shadow-map benchmark."
                        else if (testMode == 6) "The live Vulkan scene shows animated 4×4 selector patterns in ten color-coded format bands. CPU decoders still run separately and report throughput/checksums; the visualizer does not display their decoded output."
                        else if (testMode == 7) "Native log records depth-occlusion workload and frame pacing."
                        else if (testMode == 8) "Uses a real Vulkan src-alpha blend pipeline; animated transparent layers are not opaque stand-ins."
                        else if (testMode == 9) "Overlapping animated quads run the same repeated fragment arithmetic each frame to stress shader execution."
                        else if (testMode == 10) "High-count animated triangles provide a distinct primitive/raster workload; instance count is recorded in the run log."
                        else if (testMode == 11) "Dense instanced geometry exercises vertex fetch, transformation, and varying generation."
                        else if (testMode == 12) "Fragments are conditionally discarded by a time-varying pattern; the selected layer count is logged."
                        else if (testMode == 13) "A true Vulkan src-alpha blend state composites each animated translucent layer."
                        else if (testMode == 14) "The configured count is rendered as distinct animated triangles; the run log records primitive count and workload."
                        else if (testMode == 15) "One vkCmdDraw submits the configured triangle count (three vertices each), avoiding per-object draw-call overhead."
                        else "The selected scene is rendered live by the Vulkan ICD.",
                        color = Color(0xFFA9B8C9),
                        fontSize = 12.sp
                    )
                }
            }
        }

        Column {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(
                    onClick = {
                        val target = surface
                        if (target == null || !target.isValid) {
                            status = "Render surface is not ready."
                        } else {
                            running = true
                            status = "Running Vulkan benchmark…"
                            result = ""
                            reportJson = ""
                            fps = "--"
                            scoreLine = ""
                            phase = "STARTING"
                            elapsedSeconds = 0f
                            liveObjects = 0
                            liveFrames = 0L
                            presentIntervalMs = "--"
                            decoderThroughput = "--"
                            logText = ""
                            val runDuration = duration.toInt()
                            val runMode = testMode
                            val runObjects = when (runMode) {
                                2 -> objectCount.toInt().coerceIn(1024, 2048)
                                3 -> objectCount.toInt().coerceIn(1, 12)
                                4 -> 1
                                5 -> objectCount.toInt().coerceIn(2, 32)
                                6 -> objectCount.toInt().coerceIn(64, 4096)
                                7 -> objectCount.toInt().coerceIn(32, 512)
                                8 -> objectCount.toInt().coerceIn(16, 128)
                                9 -> objectCount.toInt().coerceIn(16, 128)
                                10 -> objectCount.toInt().coerceIn(1024, 4096)
                                11 -> objectCount.toInt().coerceIn(256, 2048)
                                12 -> objectCount.toInt().coerceIn(8, 64)
                                13 -> objectCount.toInt().coerceIn(16, 128)
                                14 -> objectCount.toInt().coerceIn(128, 2048)
                                15 -> objectCount.toInt().coerceIn(4096, 65536)
                                else -> objectCount.toInt().coerceIn(64, 1024)
                            }
                            val selectedTestName = testName(runMode)
                            val driverName = selectedDriver?.name ?: "Android system Vulkan"
                            val driverHash = selectedDriver?.sha256
                            val logFile = File(context.filesDir, "benchmark-current.log")
                            logFile.delete()
                            scope.launch {
                                val nativeResult = withContext(Dispatchers.IO) {
                                    Native.stopPreview()
                                    Native.setDebugLogging(debugLogging)
                                    Native.runBenchmark(
                                        selectedDriverPath,
                                        if (selectedDriver != null) {
                                            selectedDriver.file.parentFile?.absolutePath
                                                ?: File(context.filesDir, "drivers").absolutePath
                                        } else {
                                            ""
                                        },
                                        context.applicationInfo.nativeLibraryDir,
                                        target,
                                        runMode,
                                        runDuration,
                                        runObjects,
                                        logFile.absolutePath
                                    )
                                }
                                running = false
                                result = nativeResult
                                fps = "--"
                                phase = "IDLE"
                                elapsedSeconds = 0f
                                logText = logFile.takeIf { it.exists() }?.readText().orEmpty()
                                status = when (nativeResult) {
                                    "exit:0" -> "Benchmark complete."
                                    "exit:stopped" -> "Benchmark stopped by user."
                                    else -> "Benchmark failed — inspect the Vulkan error log."
                                }
                                reportTestName = selectedTestName
                                reportJson = buildReport(
                                    context,
                                    driverName,
                                    driverHash,
                                    runDuration,
                                    runObjects,
                                    selectedTestName,
                                    selectedDriver?.companionNames.orEmpty(),
                                    nativeResult,
                                    logText
                                )
                                scoreLine = logText.lineSequence()
                                    .lastOrNull { it.startsWith("SUMMARY ") }.orEmpty()
                            }
                        }
                    },
                    enabled = !running && surface?.isValid == true,
                    modifier = Modifier.weight(1f)
                ) {
                    Text(if (running) "RUNNING" else "START BENCHMARK")
                }
            }
            if (!running && reportJson.isNotEmpty()) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    OutlinedButton(
                        onClick = {
                            exportPicker.launch("benchmark-${System.currentTimeMillis()}.json")
                        },
                        modifier = Modifier.weight(1f)
                    ) { Text("Save JSON") }
                    OutlinedButton(
                        onClick = {
                            val shareIntent = Intent(Intent.ACTION_SEND).apply {
                                type = "application/json"
                                putExtra(Intent.EXTRA_SUBJECT, "BenchMark $reportTestName report")
                                putExtra(Intent.EXTRA_TEXT, reportJson)
                            }
                            context.startActivity(
                                Intent.createChooser(shareIntent, "Share full benchmark report")
                            )
                        },
                        modifier = Modifier.weight(1f)
                    ) { Text("Share Report") }
                }
            }
        }

        Column {
            Card(
                shape = RoundedCornerShape(18.dp),
                colors = CardDefaults.cardColors(containerColor = Color(0xFF111A26))
            ) {
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    Text("DRIVER", fontWeight = FontWeight.Bold, color = Color(0xFF9EB2C6))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        DriverButton("System Vulkan", !useImported, !running) { useImported = false }
                        DriverButton("Imported ICD", useImported, !running && imported != null) {
                            useImported = true
                        }
                        OutlinedButton(
                            onClick = { importPicker.launch(arrayOf("application/octet-stream", "*/*")) },
                            enabled = !running
                        ) { Text("Import ICD") }
                    }
                    Text(
                        selectedDriver?.let {
                            "${it.name}\nSHA-256 ${it.sha256}" +
                                if (it.companionNames.isEmpty()) "" else
                                    "\nPreload: ${it.companionNames.joinToString()}"
                        } ?: "Uses Android's system Vulkan loader. Choose the ICD and any companion .so files together; imports stay app-private.",
                        color = Color(0xFFA9B8C9),
                        fontSize = 12.sp,
                        lineHeight = 17.sp
                    )
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text("Detailed Vulkan debug log", fontWeight = FontWeight.SemiBold)
                            Text(
                                if (debugLogging) {
                                    "Enabled · feature/format inventories and preview frame heartbeats"
                                } else {
                                    "Disabled · essential errors and capability totals remain logged"
                                },
                                color = Color(0xFFA9B8C9),
                                fontSize = 11.sp
                            )
                        }
                        Switch(
                            checked = debugLogging,
                            onCheckedChange = { enabled ->
                                debugLogging = enabled
                                context.getSharedPreferences(
                                    "benchmark", Context.MODE_PRIVATE
                                ).edit().putBoolean("debug_logging", enabled).apply()
                            },
                            enabled = !running
                        )
                    }
                }
            }
        }

        Column {
            Card(
                shape = RoundedCornerShape(18.dp),
                colors = CardDefaults.cardColors(containerColor = Color(0xFF111A26))
            ) {
                Column(Modifier.fillMaxWidth().padding(16.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text("DRIVER / VULKAN LOG", fontWeight = FontWeight.Bold, color = Color(0xFF9EB2C6))
                        OutlinedButton(
                            onClick = {
                                logExportPicker.launch("benchmark-log-${System.currentTimeMillis()}.txt")
                            },
                            enabled = logText.isNotBlank(),
                            contentPadding = ButtonDefaults.TextButtonContentPadding
                        ) { Text("EXPORT TXT") }
                    }
                    Spacer(Modifier.height(8.dp))
                    SelectionContainer {
                        Text(
                            logText.ifEmpty { "Native ICD, device, phase and frame-time diagnostics appear here." },
                            modifier = Modifier.fillMaxWidth().height(190.dp).verticalScroll(rememberScrollState())
                                .background(Color(0xFF080D15), RoundedCornerShape(10.dp)).padding(10.dp),
                            fontFamily = FontFamily.Monospace,
                            fontSize = 10.sp,
                            lineHeight = 14.sp,
                            color = Color(0xFFCEE2E6)
                        )
                    }
                }
            }
        }

        Column {
            Text(
                "Synthetic benchmark only. It does not install or replace the system driver. Imported ICDs run inside this app process.",
                modifier = Modifier.padding(bottom = 24.dp),
                color = Color(0xFF8292A4),
                fontSize = 11.sp
            )
        }
        } else {
            val infoSource = if (running) logText else vulkanInfoText
            val infoPrefixes = when (infoCategory) {
                0 -> listOf(
                    "ICD_LOAD ", "DEVICE ", "VULKAN_PHYSICAL_DEVICE_SELECTION ",
                    "SWAPCHAIN ", "VULKAN_CAPABILITY_SUMMARY "
                )
                1 -> listOf(
                    "VULKAN_INSTANCE_SUMMARY ", "VULKAN_INSTANCE_REQUIRED_EXTENSION ",
                    "VULKAN_INSTANCE_EXTENSION ", "VULKAN_INSTANCE_LAYER "
                )
                2 -> listOf(
                    "VULKAN_DEVICE_EXTENSION ", "VULKAN_LIMITS ",
                    "VULKAN_MEMORY_HEAP ", "VULKAN_MEMORY_SUMMARY "
                )
                3 -> listOf(
                    "VULKAN_FEATURES ", "VULKAN_CAPABILITY_SUMMARY ",
                    "VULKAN_CORE_FEATURE_SUMMARY ", "CORE_FEATURE ",
                    "FEATURE_EXTENSION "
                )
                4 -> listOf(
                    "VULKAN_TEXTURE_SUMMARY ", "FORMAT_INFO ", "BC_FORMAT "
                )
                else -> emptyList()
            }
            val vulkanDetails = infoSource.lineSequence()
                .filter { line -> infoPrefixes.any(line::startsWith) }
                .joinToString("\n")
            val d3d11RuntimeFound = nativeLibraries.any {
                it.contains("dxvk_d3d11", ignoreCase = true)
            }
            val d3d12RuntimeFound = nativeLibraries.any {
                it.contains("vkd3d", ignoreCase = true)
            }
            val infoCategories = listOf(
                "Overview", "Instance", "Device", "Features", "Textures", "D3D"
            )
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                for (rowStart in infoCategories.indices step 3) {
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        for (index in rowStart until minOf(rowStart + 3, infoCategories.size)) {
                            InfoCategoryButton(
                                label = infoCategories[index],
                                selected = infoCategory == index,
                                modifier = Modifier.weight(1f)
                            ) { infoCategory = index }
                        }
                    }
                }
            }
            Column {
                Card(
                    shape = RoundedCornerShape(18.dp),
                    colors = CardDefaults.cardColors(containerColor = Color(0xFF111A26))
                ) {
                    Column(
                        Modifier.fillMaxWidth().padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(10.dp)
                    ) {
                        Text(
                            "VULKAN ${infoCategories[infoCategory].uppercase()}",
                            fontWeight = FontWeight.Bold
                        )
                        if (infoCategory != 5) {
                            Text(
                                vulkanDetails.ifEmpty {
                                    if (debugLogging && infoCategory in 1..4) {
                                        "Detailed inventory is empty. Wait for Vulkan preview initialization or start a benchmark."
                                    } else if (!debugLogging && infoCategory in 1..4) {
                                        "Enable Detailed Vulkan debug log in the Driver card to collect full per-extension, feature, and format details."
                                    } else {
                                        "Device details are not available yet. Select a Vulkan driver and wait for preview initialization."
                                    }
                                },
                                modifier = Modifier.fillMaxWidth().height(360.dp)
                                    .background(Color(0xFF080D15), RoundedCornerShape(10.dp))
                                    .padding(10.dp).verticalScroll(rememberScrollState()),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp,
                                lineHeight = 14.sp,
                                color = Color(0xFFCEE2E6)
                            )
                        } else {
                            Text("D3D TRANSLATION RUNTIME", fontWeight = FontWeight.Bold)
                            Text(
                                "D3D11 / DXVK: " + if (d3d11RuntimeFound) {
                                    "runtime library detected; execution is not integrated or validated"
                                } else {
                                    "unavailable — no DXVK D3D11 runtime bundled"
                                },
                                color = Color(0xFFA9B8C9),
                                fontSize = 12.sp
                            )
                            Text(
                                "DXVK compatibility: NOT TESTED — this app does not bundle Wine or execute Direct3D 11 workloads.",
                                color = Color(0xFFA9B8C9),
                                fontSize = 12.sp
                            )
                            Text(
                                "D3D12 / vkd3d: " + if (d3d12RuntimeFound) {
                                    "runtime library detected; execution is not integrated or validated"
                                } else {
                                    "unavailable — no vkd3d runtime bundled"
                                },
                                color = Color(0xFFA9B8C9),
                                fontSize = 12.sp
                            )
                            Text(
                                "vkd3d compatibility: NOT TESTED — this app does not bundle Wine or execute Direct3D 12 workloads.",
                                color = Color(0xFFA9B8C9),
                                fontSize = 12.sp
                            )
                            Text(
                                "Runtime presence is not proof of compatibility. A real result requires the matching runtime, a Windows D3D test executable, and a compatible Wine/Android execution layer.",
                                color = Color(0xFF8292A4),
                                fontSize = 11.sp
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun DriverButton(
    label: String,
    selected: Boolean,
    enabled: Boolean,
    onClick: () -> Unit
) {
    if (selected) {
        Button(onClick = onClick, enabled = enabled, contentPadding = ButtonDefaults.TextButtonContentPadding) {
            Text(label, fontSize = 12.sp)
        }
    } else {
        OutlinedButton(onClick = onClick, enabled = enabled, contentPadding = ButtonDefaults.TextButtonContentPadding) {
            Text(label, fontSize = 12.sp)
        }
    }
}

@Composable
private fun InfoCategoryButton(
    label: String,
    selected: Boolean,
    modifier: Modifier,
    onClick: () -> Unit
) {
    if (selected) {
        Button(
            onClick = onClick,
            modifier = modifier,
            contentPadding = ButtonDefaults.TextButtonContentPadding
        ) {
            Text(label, fontSize = 11.sp, maxLines = 1)
        }
    } else {
        OutlinedButton(
            onClick = onClick,
            modifier = modifier,
            contentPadding = ButtonDefaults.TextButtonContentPadding
        ) {
            Text(label, fontSize = 11.sp, maxLines = 1)
        }
    }
}

@Composable
private fun SettingSlider(
    title: String,
    value: Float,
    valueText: String,
    range: ClosedFloatingPointRange<Float>,
    steps: Int,
    enabled: Boolean,
    onChange: (Float) -> Unit
) {
    Column {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(title, fontWeight = FontWeight.SemiBold)
            Text(valueText, color = MaterialTheme.colorScheme.primary, fontSize = 13.sp)
        }
        Slider(value, onChange, valueRange = range, steps = steps, enabled = enabled)
    }
}

@Composable
private fun Metric(title: String, value: String) {
    Column {
        Text(title, color = Color(0xFF8292A4), fontSize = 10.sp, fontWeight = FontWeight.Bold)
        Text(value, color = Color(0xFFE8F0FA), fontSize = 17.sp, fontWeight = FontWeight.Bold)
    }
}

private fun testName(mode: Int): String = when (mode) {
    1 -> "BC1 CPU Emulation"
    2 -> "Draw Stress 1K-2K"
    3 -> "3D Dolphin"
    4 -> "Rotating Triangle"
    5 -> "Tessellation + Shadow"
    6 -> "BC2–BC7 CPU Emulation"
    7 -> "Depth / Occlusion"
    8 -> "Alpha Blend"
    9 -> "Fragment Shader Stress"
    10 -> "Triangle Raster Stress"
    11 -> "Vertex Transform Stress"
    12 -> "Fragment Discard Stress"
    13 -> "Blend Overdraw Stress"
    14 -> "Geometry Test"
    15 -> "Large Draw Test"
    else -> "Cube Scene"
}

@Composable
private fun SuiteButton(
    label: String,
    selected: Boolean,
    enabled: Boolean,
    modifier: Modifier,
    onClick: () -> Unit
) {
    if (selected) {
        Button(
            onClick = onClick,
            enabled = enabled,
            modifier = modifier,
            contentPadding = ButtonDefaults.TextButtonContentPadding
        ) {
            Text(label, fontSize = 12.sp, maxLines = 1)
        }
    } else {
        OutlinedButton(
            onClick = onClick,
            enabled = enabled,
            modifier = modifier,
            contentPadding = ButtonDefaults.TextButtonContentPadding
        ) {
            Text(label, fontSize = 12.sp, maxLines = 1)
        }
    }
}

private suspend fun importDriverBundle(context: Context, uris: List<Uri>): ImportedDriver =
    withContext(Dispatchers.IO) {
        require(uris.size in 1..8) { "Select between 1 and 8 shared libraries" }
        val entries = uris.map { uri ->
            val displayName = resolveDisplayName(context, uri)
            Triple(uri, displayName, safeLibraryName(displayName))
        }
        require(entries.map { it.third }.toSet().size == entries.size) {
            "The selected files must have unique library filenames"
        }
        val primary = entries.firstOrNull { it.second.contains("vulkan", ignoreCase = true) }
            ?: entries.first()
        val dir = File(context.filesDir, "drivers")
        if (!dir.isDirectory && !dir.mkdirs()) throw IOException("Could not create private driver directory")
        val staging = File(dir, ".import-${UUID.randomUUID()}")
        if (!staging.mkdir()) throw IOException("Could not create temporary driver bundle")
        var totalBytes = 0L

        try {
            for ((uri, displayName, safeName) in entries) {
                val target = File(staging, safeName)
                var copied = 0L
                context.contentResolver.openInputStream(uri)?.use { input ->
                    FileOutputStream(target).use { output ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            val read = input.read(buffer)
                            if (read < 0) break
                            copied += read
                            totalBytes += read
                            require(copied <= 256L * 1024 * 1024 &&
                                totalBytes <= 512L * 1024 * 1024) {
                                "Import exceeds the 512 MiB bundle limit"
                            }
                            output.write(buffer, 0, read)
                        }
                    }
                } ?: throw IOException("Could not open $displayName")

                require(copied > 0 && isArm64Elf(target)) {
                    "$displayName is not a little-endian 64-bit ARM ELF shared library"
                }
                if (!target.setReadable(true, false) ||
                    !target.setExecutable(true, false) ||
                    !target.setReadOnly()) {
                    throw IOException("Could not seal imported library $safeName as read-only")
                }
            }

            val primaryName = primary.third
            require(primaryName.endsWith(".so", ignoreCase = true)) {
                "The selected ICD must be a .so library"
            }
            val bundleDigest = MessageDigest.getInstance("SHA-256")
            bundleDigest.update(primaryName.toByteArray(Charsets.UTF_8))
            for ((_, _, name) in entries.sortedBy { it.third }) {
                bundleDigest.update(0.toByte())
                bundleDigest.update(name.toByteArray(Charsets.UTF_8))
                bundleDigest.update(sha256(File(staging, name)))
            }
            val bundleId = bundleDigest.digest()
                .joinToString("") { "%02x".format(Locale.ROOT, it) }
            val bundleDirectory = File(dir, bundleId)
            if (bundleDirectory.exists()) {
                for ((_, _, name) in entries) {
                    val existing = File(bundleDirectory, name)
                    require(existing.isFile &&
                        sha256(existing).contentEquals(sha256(File(staging, name)))) {
                        "An imported bundle hash collision was detected"
                    }
                }
            } else {
                Files.move(staging.toPath(), bundleDirectory.toPath())
            }

            val selectedTemp = File(dir, ".selected-driver-${UUID.randomUUID()}")
            selectedTemp.writeText("$bundleId/$primaryName\n")
            Files.move(
                selectedTemp.toPath(),
                File(dir, "selected-driver.txt").toPath(),
                StandardCopyOption.REPLACE_EXISTING
            )
            val driverFile = File(bundleDirectory, primaryName)
            require(driverFile.isFile) { "Selected ICD file was not copied" }
            ImportedDriver(
                driverFile,
                primaryName,
                sha256(driverFile).joinToString("") { "%02x".format(Locale.ROOT, it) },
                listOf("libdrm.so", "libc++.so")
                    .filter { File(bundleDirectory, it).isFile }
            )
        } finally {
            if (staging.exists() && !staging.deleteRecursively()) {
                throw IOException("Could not clean temporary driver bundle")
            }
        }
    }

private fun safeLibraryName(displayName: String): String {
    val name = displayName.substringAfterLast('/')
        .replace(Regex("[^A-Za-z0-9._-]"), "_")
        .take(100)
    require(name.isNotBlank() && name != "." && name != "..") {
        "Library filename is empty or invalid"
    }
    require(name.endsWith(".so", ignoreCase = true)) {
        "Only .so files can be imported"
    }
    return name
}

private fun resolveDisplayName(context: Context, uri: Uri): String =
    context.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
        val column = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
        if (column >= 0 && cursor.moveToFirst()) cursor.getString(column) else null
    } ?: uri.lastPathSegment ?: "imported-driver.so"

private fun isArm64Elf(file: File): Boolean {
    val header = ByteArray(20)
    val readHeader = file.inputStream().use { it.read(header) }
    return readHeader == header.size &&
        header[0] == 0x7f.toByte() && header[1] == 'E'.code.toByte() &&
        header[2] == 'L'.code.toByte() && header[3] == 'F'.code.toByte() &&
        header[4] == 2.toByte() && header[5] == 1.toByte() &&
        header[16] == 3.toByte() && header[17] == 0.toByte() &&
        header[18] == 183.toByte() && header[19] == 0.toByte()
}

private fun loadImportedDriver(context: Context): ImportedDriver? {
    val dir = File(context.filesDir, "drivers")
    val nameFile = File(dir, "selected-driver.txt")
    if (!nameFile.isFile) return null
    val name = nameFile.readText().trim()
    val components = name.split('/')
    if (components.size !in 1..2) return null
    val fileName = components.last()
    if (safeLibraryNameOrNull(fileName) == null) return null
    if (components.size == 2 && !components[0].matches(Regex("[0-9a-f]{64}")))
        return null
    val file = if (components.size == 2) {
        File(File(dir, components[0]), fileName)
    } else {
        File(dir, fileName)
    }
    if (!file.isFile) return null
    val hash = sha256(file).joinToString("") { "%02x".format(Locale.ROOT, it) }
    val bundleDirectory = file.parentFile ?: return null
    return ImportedDriver(
        file,
        fileName,
        hash,
        listOf("libdrm.so", "libc++.so")
            .filter { File(bundleDirectory, it).isFile }
    )
}

private fun safeLibraryNameOrNull(name: String): String? =
    name.takeIf {
        it.isNotBlank() &&
            it != "." &&
            it != ".." &&
            it.length <= 100 &&
            it.matches(Regex("[A-Za-z0-9._-]+")) &&
            it.endsWith(".so", ignoreCase = true)
    }

private fun sha256(file: File): ByteArray {
    val digest = MessageDigest.getInstance("SHA-256")
    file.inputStream().buffered().use { input ->
        val buffer = ByteArray(64 * 1024)
        while (true) {
            val read = input.read(buffer)
            if (read < 0) break
            digest.update(buffer, 0, read)
        }
    }
    return digest.digest()
}

private fun buildReport(
    context: Context,
    driverName: String,
    driverHash: String?,
    durationSeconds: Int,
    objects: Int,
    testName: String,
    companions: List<String>,
    nativeResult: String,
    log: String
): String = JSONObject()
    .put("schema", "benchmark-report-v2")
    .put("createdAt", System.currentTimeMillis())
    .put("device", "${Build.MANUFACTURER} ${Build.MODEL}")
    .put("androidRelease", Build.VERSION.RELEASE)
    .put("appVersion", "0.3.0")
    .put("driver", driverName)
    .put("driverSha256", driverHash ?: JSONObject.NULL)
    .put("driverCompanions", org.json.JSONArray(companions))
    .put("thermalStatus", context.getSystemService(android.os.PowerManager::class.java)
        ?.currentThermalStatus ?: JSONObject.NULL)
    .put("durationSeconds", durationSeconds)
    .put("testSuite", testName)
    .put("sceneObjects", objects)
    .put("nativeResult", nativeResult)
    .put("log", log)
    .toString(2)
