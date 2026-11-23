package com.ht.turret;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import java.util.UUID;

/**
 * Unity 用的 BLE 插件：只负责"连上 C5 并发 16 字节帧"，不含 UI。
 * Unity 侧用 AndroidJavaClass("com.ht.turret.BleLink") 调用。
 *
 * 和网页版协议逐字节一致（见 docs/FIRMWARE.md 第 3 节）。
 */
public class BleLink {

    private static final String TAG = "BleLink";
    private static final UUID SVC    = UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb");
    private static final UUID CH_WR   = UUID.fromString("0000fff1-0000-1000-8000-00805f9b34fb");
    private static final UUID CH_NTF  = UUID.fromString("0000fff2-0000-1000-8000-00805f9b34fb");

    private static BleLink sInst;

    private final Context ctx;
    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic chWrite;

    /** 给 Unity 读的状态：0=空闲 1=扫描 2=连接中 3=就绪 -1=错误 */
    private volatile int state = 0;
    private volatile int sentCount = 0;
    private volatile String lastError = "";

    private final Activity act;

    private BleLink(Activity a) { this.act = a; this.ctx = a.getApplicationContext(); }

    /** Unity 入口：Awake 里调一次 */
    public static BleLink create(Activity activity) {
        if (sInst == null) sInst = new BleLink(activity);
        return sInst;
    }

    public static BleLink instance() { return sInst; }

    public int getState()      { return state; }
    public int getSentCount()  { return sentCount; }
    public String getError()   { return lastError; }
    public boolean isReady()   { return chWrite != null && state == 3; }

    /**
     * 请求蓝牙运行时权限。Android 12+ 必须动态请求，清单里声明了也不够。
     * Unity 那边会隔一会儿再调 begin()，所以这里只负责弹框。
     */
    @SuppressLint("MissingPermission")
    private void requestBtPerms() {
        if (Build.VERSION.SDK_INT >= 31) {
            act.runOnUiThread(() -> act.requestPermissions(new String[]{
                    android.Manifest.permission.BLUETOOTH_SCAN,
                    android.Manifest.permission.BLUETOOTH_CONNECT}, 9001));
            lastError = "已弹出蓝牙权限请求，请在头显里点允许";
            state = -2;
        }
    }

    @SuppressLint("MissingPermission")
    public boolean begin(String namePrefix) {
        if (adapter == null) {
            BluetoothManager bm = (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            adapter = bm != null ? bm.getAdapter() : null;
        }
        if (adapter == null || !adapter.isEnabled()) { lastError = "蓝牙未开启"; state = -1; return false; }

        if (Build.VERSION.SDK_INT >= 31) {
            if (ctx.checkSelfPermission(android.Manifest.permission.BLUETOOTH_SCAN)
                    != PackageManager.PERMISSION_GRANTED) {
                if (state != -2) requestBtPerms();     /* 只弹一次 */
                lastError = "缺少 BLUETOOTH_SCAN 权限";
                state = -2;
                return false;
            }
        }

        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) { lastError = "扫描器不可用"; state = -1; return false; }

        ScanSettings st = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        state = 1;
        scanner.startScan(null, st, cb);
        Log.i(TAG, "开始扫描 " + namePrefix);
        return true;
    }

    @SuppressLint("MissingPermission")
    public void stop() {
        if (scanner != null) { try { scanner.stopScan(cb); } catch (Exception ignored) {} }
        if (gatt != null)    { try { gatt.close(); } catch (Exception ignored) {} }
        gatt = null; chWrite = null; state = 0;
    }

    private final ScanCallback cb = new ScanCallback() {
        @SuppressLint("MissingPermission")
        @Override public void onScanResult(int type, ScanResult r) {
            String n = r.getDevice().getName();
            if (n == null || !n.startsWith("C5-")) return;
            try { scanner.stopScan(cb); } catch (Exception ignored) {}
            state = 2;
            gatt = r.getDevice().connectGatt(ctx, false, gcb);
            Log.i(TAG, "连接 " + n);
        }
    };

    private final BluetoothGattCallback gcb = new BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        @Override public void onConnectionStateChange(BluetoothGatt g, int st, int ns) {
            if (ns == BluetoothProfile.STATE_CONNECTED) {
                g.discoverServices();
            } else {
                chWrite = null; state = 0;
                lastError = "断开 status=" + st;
            }
        }

        @SuppressLint("MissingPermission")
        @Override public void onServicesDiscovered(BluetoothGatt g, int st) {
            BluetoothGattService s = g.getService(SVC);
            if (s == null) { lastError = "无 FFF0 服务"; state = -1; return; }
            chWrite = s.getCharacteristic(CH_WR);
            BluetoothGattCharacteristic ntf = s.getCharacteristic(CH_NTF);
            if (ntf != null) g.setCharacteristicNotification(ntf, true);
            state = chWrite != null ? 3 : -1;
            if (chWrite == null) lastError = "无 FFF1 特征";
        }
    };

    /**
     * 发一帧。16 字节小端，CRC16-CCITT 覆盖前 14 字节。
     * 从 Unity 每帧调用即可，内部用无响应写（最快）。
     */
    @SuppressLint("MissingPermission")
    public boolean send(short yawCdeg, short pitchCdeg, short yawRateDps10,
                        int seq, int flags) {
        if (chWrite == null || gatt == null) return false;

        byte[] b = new byte[16];
        put16(b, 0, 0xA55A);
        put16(b, 2, seq & 0xFFFF);
        put16(b, 4, yawCdeg);
        put16(b, 6, pitchCdeg);
        put16(b, 8, yawRateDps10);
        put16(b, 10, 0);
        put16(b, 12, flags);
        put16(b, 14, crc16(b, 14));

        chWrite.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
        chWrite.setValue(b);
        boolean ok = gatt.writeCharacteristic(chWrite);
        if (ok) sentCount++;
        return ok;
    }

    private static void put16(byte[] b, int off, int v) {
        b[off] = (byte) (v & 0xFF);
        b[off + 1] = (byte) ((v >> 8) & 0xFF);
    }

    static int crc16(byte[] d, int len) {
        int crc = 0xFFFF;
        for (int i = 0; i < len; i++) {
            crc ^= (d[i] & 0xFF) << 8;
            for (int k = 0; k < 8; k++)
                crc = ((crc & 0x8000) != 0) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
        }
        return crc;
    }
}
