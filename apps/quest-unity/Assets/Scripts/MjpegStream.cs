using System;
using UnityEngine;
using UnityEngine.Networking;

/// <summary>
/// MJPEG 流客户端：一条 HTTP 长连接里持续解析 multipart 边界，
/// 每凑齐一帧 JPEG 就回调一次。
///
/// 相比"每帧一次 GET /snapshot.jpg"：
///   · 省掉每帧 TCP/HTTP 开销
///   · 帧率能到 25~30 fps
///   · PC 端由 devserver.py 的 /stream.mjpg 提供
/// </summary>
public class MjpegStream : DownloadHandlerScript
{
    readonly byte[] _buf = new byte[8 * 1024 * 1024];
    int _len;

    public Texture2D Texture { get; private set; }
    public int FrameCount { get; private set; }
    public long TotalBytes { get; private set; }
    public int LastWidth { get; private set; }
    public int LastHeight { get; private set; }
    public string LastError { get; private set; } = "-";

    public MjpegStream()
    {
        Texture = new Texture2D(2, 2, TextureFormat.RGBA32, false);
        Texture.wrapMode = TextureWrapMode.Clamp;
    }

    protected override bool ReceiveData(byte[] data, int dataLength)
    {
        if (data == null || dataLength <= 0) return true;
        TotalBytes += dataLength;

        /* 追加到缓冲；满了就把旧数据丢掉（说明没找到边界） */
        if (_len + dataLength > _buf.Length)
        {
            if (dataLength > _buf.Length) { _len = 0; return true; }
            int keep = _buf.Length - dataLength;
            Buffer.BlockCopy(_buf, _len - keep, _buf, 0, keep);
            _len = keep;
        }

        Buffer.BlockCopy(data, 0, _buf, _len, dataLength);
        _len += dataLength;

        /* 找 SOI(FFD8) ... EOI(FFD9) */
        int start = IndexOf(_buf, _len, 0xFF, 0xD8, 0, _len);
        while (start >= 0)
        {
            int end = IndexOf(_buf, _len, 0xFF, 0xD9, start + 2, _len);
            if (end < 0) break;                      /* 等下一批数据 */

            int jpegLen = end + 2 - start;
            if (jpegLen > 100)
            {
                var jpg = new byte[jpegLen];
                Buffer.BlockCopy(_buf, start, jpg, 0, jpegLen);
                try
                {
                    if (Texture.LoadImage(jpg))
                    {
                        FrameCount++;
                        LastWidth = Texture.width;
                        LastHeight = Texture.height;
                        LastError = "-";
                    }
                }
                catch (Exception e) { LastError = e.Message; }
            }

            /* 丢掉已处理部分 */
            int rest = _len - (end + 2);
            if (rest > 0) Buffer.BlockCopy(_buf, end + 2, _buf, 0, rest);
            _len = rest;

            start = IndexOf(_buf, _len, 0xFF, 0xD8, 0, _len);
        }

        return true;
    }

    static int IndexOf(byte[] b, int len, byte b1, byte b2, int from, int to)
    {
        for (int i = from; i < to - 1 && i < len - 1; i++)
            if (b[i] == b1 && b[i + 1] == b2) return i;
        return -1;
    }
}
