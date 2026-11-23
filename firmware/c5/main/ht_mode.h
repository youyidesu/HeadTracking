#pragma once
/* 构建模式：1 = 网页版（WiFi+HTTPS+WebSocket），2 = App 版（纯 BLE）
 *
 * 例程里的 build.ps1 会在编译前改写这个文件，平时不用手动动。
 * 想手动切：改下面这个数字，然后 idf.py build。 */
#define HT_MODE 2
