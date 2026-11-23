# -*- coding: utf-8 -*-
"""把 device_filter 打成一个最小 AAR。

Unity 6 不再支持 Assets/Plugins/Android/res（会直接报 OBSOLETE 并中断构建），
官方要求把 Android 资源放进 AAR。这里用 Python 手工造一个只含
res/xml/device_filter.xml 的 AAR，避开 Gradle/AAPT 依赖。
"""
import os
import zipfile

ROOT = r"D:\project\HeadTracking.Bridge\apps\quest-unity\HTSender\Assets\Plugins\Android"
AAR = os.path.join(ROOT, "usbfilter.aar")

MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.ht.turret.usbfilter">
</manifest>
"""

FILTER = """<?xml version="1.0" encoding="utf-8"?>
<resources>
    <usb-device vendor-id="11145" product-id="21320" />
</resources>
"""

with zipfile.ZipFile(AAR, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("AndroidManifest.xml", MANIFEST)
    z.writestr("res/xml/device_filter.xml", FILTER)
    # AAR 里 classes.jar 可以为空，但有些工具链要求存在
    z.writestr("classes.jar", b"")

print("已生成", AAR, os.path.getsize(AAR), "字节")

# 删掉不再允许的 res 目录（否则构建直接失败）
res_dir = os.path.join(ROOT, "res")
if os.path.isdir(res_dir):
    import shutil
    shutil.rmtree(res_dir)
    print("已删除 Assets/Plugins/Android/res")
