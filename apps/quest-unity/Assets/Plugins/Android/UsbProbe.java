package com.ht.turret;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * UVC 采集卡探测（只读描述符，不读视频流）。
 *
 * 目的：确认 Q3 能不能打开这块卡、它有哪些接口/端点、
 * HDMI 输入后端点是否能被 claim。这些是写 libuvc 前必须知道的。
 *
 * 用法（Unity 侧反射调用）：
 *   UsbProbe.init(activity);
 *   UsbProbe.dump();          // 结果写 app.log，也进 logcat
 */
public class UsbProbe {

    private static final String TAG = "UsbProbe";
    private static final String ACTION_USB_PERM = "com.ht.turret.USB_PERM";

    private static Activity sActivity;
    private static UsbManager sManager;
    private static UsbDevice sDevice;

    public static void init(Activity activity) {
        sActivity = activity;
        sManager = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
    }

    /** 把一行写进 app.log（和 Unity 共用同一个文件） */
    private static void log(String msg) {
        Log.i(TAG, msg);
        try {
            File f = new File(sActivity.getExternalFilesDir(null), "app.log");
            FileWriter w = new FileWriter(f, true);
            String t = new SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(new Date());
            w.write(t + "  [UsbProbe] " + msg + "\n");
            w.close();
        } catch (IOException e) {
            Log.w(TAG, "写日志失败: " + e.getMessage());
        }
    }

    /** 枚举所有 USB 设备，挑第一个 class 为 239(UVC IAD) 或含视频接口的 */
    public static void dump() {
        if (sManager == null) { log("UsbManager 为空"); return; }

        log("设备数 " + sManager.getDeviceList().size());

        for (UsbDevice d : sManager.getDeviceList().values()) {
            log(String.format(Locale.US, "设备 vid=0x%04x pid=0x%04x class=%d ifCount=%d name=%s",
                    d.getVendorId(), d.getProductId(), d.getDeviceClass(),
                    d.getInterfaceCount(), d.getDeviceName()));

            if (d.getVendorId() != 0x2b89 || d.getProductId() != 0x5348) continue;
            sDevice = d;
        }

        if (sDevice == null) { log("没找到采集卡 2b89:5348"); return; }

        boolean has = sManager.hasPermission(sDevice);
        log("权限: " + (has ? "已有" : "无，申请中"));

        if (!has) {
            PendingIntent pi = PendingIntent.getBroadcast(sActivity, 0,
                    new Intent(ACTION_USB_PERM).setPackage(sActivity.getPackageName()),
                    Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
            sManager.requestPermission(sDevice, pi);
            return;   // 授权回调里再继续
        }

        describe();
    }

    /** 打印接口与端点 */
    private static void describe() {
        if (sDevice == null) return;

        UsbDeviceConnection conn = sManager.openDevice(sDevice);
        log("openDevice -> " + (conn != null ? "成功" : "失败"));

        for (int i = 0; i < sDevice.getInterfaceCount(); i++) {
            UsbInterface itf = sDevice.getInterface(i);
            log(String.format(Locale.US, "接口%d class=%d sub=%d proto=%d 端点=%d",
                    i, itf.getInterfaceClass(), itf.getInterfaceSubclass(),
                    itf.getInterfaceProtocol(), itf.getEndpointCount()));

            for (int e = 0; e < itf.getEndpointCount(); e++) {
                UsbEndpoint ep = itf.getEndpoint(e);
                log(String.format(Locale.US, "  端点 addr=0x%02x type=%d dir=%s maxPacket=%d interval=%d",
                        ep.getAddress(), ep.getType(),
                        ep.getDirection() == UsbConstants.USB_DIR_IN ? "IN" : "OUT",
                        ep.getMaxPacketSize(), ep.getInterval()));
            }

            if (conn != null) {
                boolean ok = conn.claimInterface(itf, true);
                log("  claimInterface(" + i + ") -> " + ok);
                if (ok) conn.releaseInterface(itf);
            }
        }

        if (conn != null) conn.close();
        log("描述符探测完成");
    }

    /** 授权结果回调（Unity 侧注册一次即可） */
    public static void registerPermissionReceiver() {
        if (sActivity == null) return;

        IntentFilter f = new IntentFilter(ACTION_USB_PERM);
        if (Build.VERSION.SDK_INT >= 33) {
            sActivity.registerReceiver(new BroadcastReceiver() {
                @Override public void onReceive(Context c, Intent i) {
                    boolean granted = i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                    log("权限回调: " + granted);
                    if (granted) describe();
                }
            }, f, Context.RECEIVER_NOT_EXPORTED);
        } else {
            sActivity.registerReceiver(new BroadcastReceiver() {
                @Override public void onReceive(Context c, Intent i) {
                    boolean granted = i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                    log("权限回调: " + granted);
                    if (granted) describe();
                }
            }, f);
        }
    }
}
