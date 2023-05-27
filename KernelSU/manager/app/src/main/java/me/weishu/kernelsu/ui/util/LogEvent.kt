package me.weishu.kernelsu.ui.util

import android.content.Context
import android.os.Build
import android.system.Os
import com.topjohnwu.superuser.ShellUtils
import me.weishu.kernelsu.Natives
import me.weishu.kernelsu.ui.screen.getManagerVersion
import java.io.File
import java.io.FileWriter
import java.io.PrintWriter
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

fun getBugreportFile(context: Context): File {

    val bugreportDir = File(context.cacheDir, "bugreport")
    bugreportDir.mkdirs()

    val dmesgFile = File(bugreportDir, "dmesg.txt")
    val logcatFile = File(bugreportDir, "logcat.txt")
    val tombstonesFile = File(bugreportDir, "tombstones.tar.gz")
    val dropboxFile = File(bugreportDir, "dropbox.tar.gz")
    val pstoreFile = File(bugreportDir, "pstore.tar.gz")
    val diagFile = File(bugreportDir, "diag.tar.gz")
    val bootlogFile = File(bugreportDir, "bootlog.tar.gz")
    val mountsFile = File(bugreportDir, "mounts.txt")
    val fileSystemsFile = File(bugreportDir, "filesystems.txt")
    val ksuFileTree = File(bugreportDir, "ksu_tree.txt")
    val appListFile = File(bugreportDir, "packages.txt")
    val propFile = File(bugreportDir, "props.txt")
    val allowListFile = File(bugreportDir, "allowlist.bin")

    val shell = getRootShell()

    shell.newJob().add("dmesg > ${dmesgFile.absolutePath}").exec()
    shell.newJob().add("logcat -d > ${logcatFile.absolutePath}").exec()
    shell.newJob().add("tar -czf ${tombstonesFile.absolutePath} -C /data/tombstones .").exec()
    shell.newJob().add("tar -czf ${dropboxFile.absolutePath} -C /data/system/dropbox .").exec()
    shell.newJob().add("tar -czf ${pstoreFile.absolutePath} -C /sys/fs/pstore .").exec()
    shell.newJob().add("tar -czf ${diagFile.absolutePath} -C /data/vendor/diag .").exec()
    shell.newJob().add("tar -czf ${bootlogFile.absolutePath} -C /data/adb/ksu/log .").exec()

    shell.newJob().add("cat /proc/1/mountinfo > ${mountsFile.absolutePath}").exec()
    shell.newJob().add("cat /proc/filesystems > ${fileSystemsFile.absolutePath}").exec()
    shell.newJob().add("ls -alRZ /data/adb > ${ksuFileTree.absolutePath}").exec()
    shell.newJob().add("cp /data/system/packages.list ${appListFile.absolutePath}").exec()
    shell.newJob().add("getprop > ${propFile.absolutePath}").exec()
    shell.newJob().add("cp /data/adb/ksu/.allowlist ${allowListFile.absolutePath}").exec()

    val selinux = ShellUtils.fastCmd(shell, "getenforce");

    // basic information
    val buildInfo = File(bugreportDir, "basic.txt")
    PrintWriter(FileWriter(buildInfo)).use { pw ->
        pw.println("Kernel: ${System.getProperty("os.version")}")
        pw.println("BRAND: " + Build.BRAND)
        pw.println("MODEL: " + Build.MODEL)
        pw.println("PRODUCT: " + Build.PRODUCT)
        pw.println("MANUFACTURER: " + Build.MANUFACTURER)
        pw.println("SDK: " + Build.VERSION.SDK_INT)
        pw.println("PREVIEW_SDK: " + Build.VERSION.PREVIEW_SDK_INT)
        pw.println("FINGERPRINT: " + Build.FINGERPRINT)
        pw.println("DEVICE: " + Build.DEVICE)
        pw.println("Manager: " + getManagerVersion(context))
        pw.println("SELinux: $selinux")

        val uname = Os.uname()
        pw.println("KernelRelease: ${uname.release}")
        pw.println("KernelVersion: ${uname.version}")
        pw.println("Mahcine: ${uname.machine}")
        pw.println("Nodename: ${uname.nodename}")
        pw.println("Sysname: ${uname.sysname}")

        val ksuKernel = Natives.getVersion()
        pw.println("KernelSU: $ksuKernel")
        val safeMode = Natives.isSafeMode()
        pw.println("SafeMode: $safeMode")
    }

    // modules
    val modulesFile = File(bugreportDir, "modules.json")
    modulesFile.writeText(listModules())

    val formatter = DateTimeFormatter.ofPattern("yyyy-MM-dd_HH_mm")
    val current = LocalDateTime.now().format(formatter)

    val targetFile = File(context.cacheDir, "KernelSU_bugreport_${current}.tar.gz")

    shell.newJob().add("tar czf ${targetFile.absolutePath} -C ${bugreportDir.absolutePath} .").exec()
    shell.newJob().add("rm -rf ${bugreportDir.absolutePath}").exec()
    shell.newJob().add("chmod 0644 ${targetFile.absolutePath}").exec()

    return targetFile
}
