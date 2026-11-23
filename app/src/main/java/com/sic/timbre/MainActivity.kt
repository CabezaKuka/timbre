package com.sic.timbre

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val client = TimbreClient(applicationContext)
        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    TimbreScreen(client)
                }
            }
        }
    }
}

@Composable
fun TimbreScreen(client: TimbreClient) {
    val scope = rememberCoroutineScope()
    val haptic = LocalHapticFeedback.current

    var device by remember { mutableStateOf(client.saved) }
    var working by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("") }
    var isError by remember { mutableStateOf(false) }
    var choices by remember { mutableStateOf<List<Device>?>(null) }

    fun show(r: RingResult) {
        when (r) {
            is RingResult.Ok -> {
                device = r.device
                isError = false
                status = if (r.alreadyRinging) "Ya está sonando" else "¡Timbre activado!"
                haptic.performHapticFeedback(HapticFeedbackType.LongPress)
            }
            is RingResult.Choose -> {
                isError = false
                status = "Hay varios timbres en esta red: seleccione uno"
                choices = r.devices
            }
            is RingResult.Error -> {
                isError = true
                status = r.message
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            text = device?.name ?: "Ningún timbre seleccionado",
            fontSize = 22.sp,
            fontWeight = FontWeight.Bold
        )
        Text(
            text = device?.ip ?: "Se busca automáticamente al tocar",
            fontSize = 13.sp,
            color = Color.Gray
        )

        Spacer(Modifier.height(40.dp))

        Button(
            onClick = {
                if (!working) {
                    working = true
                    status = "Enviando..."
                    isError = false
                    scope.launch {
                        show(client.ring())
                        working = false
                    }
                }
            },
            shape = CircleShape,
            modifier = Modifier.size(240.dp),
            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF1565C0))
        ) {
            if (working) {
                CircularProgressIndicator(color = Color.White, modifier = Modifier.size(56.dp))
            } else {
                Text("TOCAR\nTIMBRE", fontSize = 30.sp, fontWeight = FontWeight.Bold, textAlign = TextAlign.Center)
            }
        }

        Spacer(Modifier.height(28.dp))

        Text(
            text = status,
            fontSize = 17.sp,
            textAlign = TextAlign.Center,
            color = if (isError) Color(0xFFC62828) else Color(0xFF2E7D32),
            modifier = Modifier.fillMaxWidth().height(52.dp)
        )

        Spacer(Modifier.height(24.dp))

        OutlinedButton(
            enabled = !working,
            onClick = {
                working = true
                isError = false
                status = "Buscando timbres..."
                scope.launch {
                    val list = client.discover()
                    working = false
                    when {
                        list == null -> { isError = true; status = "El teléfono no está conectado a WiFi" }
                        list.isEmpty() -> { isError = true; status = "No se encontró ningún timbre en esta red" }
                        else -> { status = ""; choices = list }
                    }
                }
            }
        ) {
            Text("Buscar / cambiar timbre")
        }
    }

    choices?.let { list ->
        AlertDialog(
            onDismissRequest = { choices = null },
            title = { Text("Seleccionar timbre") },
            text = {
                Column {
                    list.forEach { d ->
                        TextButton(
                            onClick = {
                                client.saved = d
                                device = d
                                choices = null
                                isError = false
                                status = "Seleccionado: ${d.name}"
                            },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text("${d.name}   (${d.ip})", modifier = Modifier.fillMaxWidth())
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { choices = null }) { Text("Cancelar") }
            }
        )
    }
}
