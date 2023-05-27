package me.weishu.kernelsu.ui.screen

import android.content.Context
import android.os.Build
import android.os.PowerManager
import android.system.Os
import androidx.annotation.StringRes
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.outlined.Block
import androidx.compose.material.icons.outlined.CheckCircle
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootNavGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import me.weishu.kernelsu.*
import me.weishu.kernelsu.R
import me.weishu.kernelsu.ui.screen.destinations.SettingScreenDestination
import me.weishu.kernelsu.ui.util.*

@RootNavGraph(start = true)
@Destination
@Composable
fun HomeScreen(navigator: DestinationsNavigator) {
    Scaffold(
        topBar = {
            TopBar(onSettingsClick = {
                navigator.navigate(SettingScreenDestination)
            })
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .padding(innerPadding)
                .padding(horizontal = 16.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            val kernelVersion = getKernelVersion()
            val isManager = Natives.becomeManager(ksuApp.packageName)
            SideEffect {
                if (isManager) install()
            }
            val ksuVersion = if (isManager) Natives.getVersion() else null

            StatusCard(kernelVersion, ksuVersion)
            InfoCard()
            DonateCard()
            LearnMoreCard()
            Spacer(Modifier)
        }
    }
}

@Composable
fun RebootDropdownItem(@StringRes id: Int, reason: String = "") {
    DropdownMenuItem(text = {
        Text(stringResource(id))
    }, onClick = {
        reboot(reason)
    })
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(onSettingsClick: () -> Unit) {
    TopAppBar(
        title = { Text(stringResource(R.string.app_name)) },
        actions = {
            var showDropdown by remember { mutableStateOf(false) }
            IconButton(onClick = {
                showDropdown = true
            }) {
                Icon(
                    imageVector = Icons.Filled.Refresh,
                    contentDescription = stringResource(id = R.string.reboot)
                )

                DropdownMenu(expanded = showDropdown, onDismissRequest = {
                    showDropdown = false
                }) {

                    RebootDropdownItem(id = R.string.reboot)

                    val pm =
                        LocalContext.current.getSystemService(Context.POWER_SERVICE) as PowerManager?
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && pm?.isRebootingUserspaceSupported == true) {
                        RebootDropdownItem(id = R.string.reboot_userspace, reason = "userspace")
                    }
                    RebootDropdownItem(id = R.string.reboot_recovery, reason = "recovery")
                    RebootDropdownItem(id = R.string.reboot_bootloader, reason = "bootloader")
                    RebootDropdownItem(id = R.string.reboot_download, reason = "download")
                    RebootDropdownItem(id = R.string.reboot_edl, reason = "edl")
                }
            }

            IconButton(onClick = onSettingsClick) {
                Icon(
                    imageVector = Icons.Filled.Settings,
                    contentDescription = stringResource(id = R.string.settings)
                )
            }
        }
    )
}

@Composable
private fun StatusCard(kernelVersion: KernelVersion, ksuVersion: Int?) {
    ElevatedCard(
        colors = CardDefaults.elevatedCardColors(containerColor = run {
            if (ksuVersion != null) MaterialTheme.colorScheme.secondaryContainer
            else MaterialTheme.colorScheme.errorContainer
        })
    ) {
        val uriHandler = LocalUriHandler.current
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable {
                    if (kernelVersion.isGKI() && ksuVersion == null) {
                        uriHandler.openUri("https://kernelsu.org/guide/installation.html")
                    }
                }
                .padding(24.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            when {
                ksuVersion != null -> {
                    val appendText = if (Natives.isSafeMode()) {
                        " [${stringResource(id = R.string.safe_mode)}]"
                    } else {
                        ""
                    }
                    Icon(Icons.Outlined.CheckCircle, stringResource(R.string.home_working))
                    Column(Modifier.padding(start = 20.dp)) {
                        Text(
                            text = stringResource(R.string.home_working) + appendText,
                            style = MaterialTheme.typography.titleMedium
                        )
                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = stringResource(R.string.home_working_version, ksuVersion),
                            style = MaterialTheme.typography.bodyMedium
                        )
                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = stringResource(R.string.home_superuser_count, getSuperuserCount()),
                            style = MaterialTheme.typography.bodyMedium
                        )
                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = stringResource(R.string.home_module_count, getModuleCount()),
                            style = MaterialTheme.typography.bodyMedium
                        )
                    }
                }
                kernelVersion.isGKI() -> {
                    Icon(Icons.Outlined.Warning, stringResource(R.string.home_not_installed))
                    Column(Modifier.padding(start = 20.dp)) {
                        Text(
                            text = stringResource(R.string.home_not_installed),
                            style = MaterialTheme.typography.titleMedium
                        )
                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = stringResource(R.string.home_click_to_install),
                            style = MaterialTheme.typography.bodyMedium
                        )
                    }
                }
                else -> {
                    Icon(Icons.Outlined.Block, stringResource(R.string.home_unsupported))
                    Column(Modifier.padding(start = 20.dp)) {
                        Text(
                            text = stringResource(R.string.home_unsupported),
                            fontFamily = FontFamily.Serif,
                            style = MaterialTheme.typography.titleMedium
                        )
                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = stringResource(R.string.home_unsupported_reason),
                            style = MaterialTheme.typography.bodyMedium
                        )
                    }
                }
            }
        }
    }
}

@Composable
fun LearnMoreCard() {
    val uriHandler = LocalUriHandler.current
    val url = stringResource(R.string.home_learn_kernelsu_url)

    ElevatedCard {

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable {
                    uriHandler.openUri(url)
                }
                .padding(24.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column() {
                Text(
                    text = stringResource(R.string.home_learn_kernelsu),
                    style = MaterialTheme.typography.titleSmall
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    text = stringResource(R.string.home_click_to_learn_kernelsu),
                    style = MaterialTheme.typography.bodyMedium
                )
            }
        }
    }
}

@Composable
fun DonateCard() {
    val uriHandler = LocalUriHandler.current

    ElevatedCard {

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable {
                    uriHandler.openUri("https://patreon.com/weishu")
                }
                .padding(24.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column() {
                Text(
                    text = stringResource(R.string.home_support_title),
                    style = MaterialTheme.typography.titleSmall
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    text = stringResource(R.string.home_support_content),
                    style = MaterialTheme.typography.bodyMedium
                )
            }
        }
    }
}

@Composable
private fun InfoCard() {
    val context = LocalContext.current

    ElevatedCard {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 24.dp, top = 24.dp, end = 24.dp, bottom = 16.dp)
        ) {
            val contents = StringBuilder()
            val uname = Os.uname()

            @Composable
            fun InfoCardItem(label: String, content: String) {
                contents.appendLine(label).appendLine(content).appendLine()
                Text(text = label, style = MaterialTheme.typography.bodyLarge)
                Text(text = content, style = MaterialTheme.typography.bodyMedium)
            }

            InfoCardItem(stringResource(R.string.home_kernel), uname.release)

            Spacer(Modifier.height(16.dp))
            InfoCardItem(stringResource(R.string.home_manager_version), getManagerVersion(context))

            Spacer(Modifier.height(16.dp))
            InfoCardItem(stringResource(R.string.home_fingerprint), Build.FINGERPRINT)

            Spacer(Modifier.height(16.dp))
            InfoCardItem(stringResource(R.string.home_selinux_status), getSELinuxStatus())
        }
    }
}

fun getManagerVersion(context: Context): String {
    val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)
    return "${packageInfo.versionName} (${packageInfo.versionCode})"
}

@Preview
@Composable
private fun StatusCardPreview() {
    Column {
        StatusCard(KernelVersion(5, 10, 101), 1)
        StatusCard(KernelVersion(5, 10, 101), null)
        StatusCard(KernelVersion(4, 10, 101), null)
    }
}
