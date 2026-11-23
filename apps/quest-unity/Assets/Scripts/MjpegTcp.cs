using System;
using System.Collections.Concurrent;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using UnityEngine;

/// <summary>
/// 自己开 TCP 读 MJPEG 流，不依赖 UnityWebRequest。
///
/// 为什么换掉 UnityWebRequest：
///   实测在 Quest 上 DownloadHandlerScript 只回调一两次就停了（bytes 卡在一帧），
///   而同一台机器的服务端用 curl 拉是连续的 —— 说明卡在 Unity 的下载器上。
///   自己用 Socket 读，行为完全可控，还能放到后台线程解析。
/// </summary>
public class MjpegTcp : MonoBehaviour
{
    public string host = "127.0.0.1";
    public int port = 8080;
    public string path = "/stream.mjpg";

    public Texture2D Texture { get; private set; }
    public int FrameCount { get; private set; }
    public long TotalBytes { get; private set; }
    public string LastError { get; private set; } = "-";
    public bool Connected { get; private set; }
    public long SoiSeen { get; private set; }      /* 收到过多少个 SOI */
    public long LoadFail { get; private set; }     /* LoadImage 失败次数 */
    public string HeaderHex { get; private set; } = "-";

    readonly ConcurrentQueue<byte[]> _queue = new ConcurrentQueue<byte[]>();
    Thread _thread;
    volatile bool _run;
    TcpClient _client;

    void Awake()
    {
        Texture = new Texture2D(2, 2, TextureFormat.RGBA32, false);
        Texture.wrapMode = TextureWrapMode.Clamp;
    }

    void OnEnable() { StartStream(); }

    void OnDisable() { StopStream(); }

    public void StartStream()
    {
        if (_run) return;
        _run = true;
        _thread = new Thread(Loop) { IsBackground = true };
        _thread.Start();
    }

    public void StopStream()
    {
        _run = false;
        try { _client?.Close(); } catch { }
        _client = null;
    }

    void Loop()
    {
        while (_run)
        {
            try
            {
                _client = new TcpClient();
                _client.NoDelay = true;
                _client.Connect(host, port);
                Connected = true;
                LastError = "-";

                using (var ns = _client.GetStream())
                {
                    string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + port +
                                 "\r\nConnection: close\r\n\r\n";
                    var reqBytes = Encoding.ASCII.GetBytes(req);
                    ns.Write(reqBytes, 0, reqBytes.Length);

                    var buf = new byte[256 * 1024];
                    var acc = new System.IO.MemoryStream();
                    int headerEnd = 0;
                    bool headerDone = false;

                    while (_run)
                    {
                        int n = ns.Read(buf, 0, buf.Length);
                        if (n <= 0) break;

                        TotalBytes += n;
                        acc.Write(buf, 0, n);

                        if (!headerDone)
                        {
                            var arr0 = acc.ToArray();
                            int idx = IndexOfHeaderEnd(arr0);
                            if (idx < 0) continue;
                            HeaderHex = System.BitConverter.ToString(arr0, 0, System.Math.Min(48, arr0.Length));
                            headerDone = true;
                            headerEnd = idx + 4;

                            var rest = new byte[arr0.Length - headerEnd];
                            Buffer.BlockCopy(arr0, headerEnd, rest, 0, rest.Length);
                            acc.SetLength(0);
                            acc.Write(rest, 0, rest.Length);
                        }

                        ExtractFrames(acc);
                    }
                }
            }
            catch (Exception e)
            {
                if (_run) { LastError = e.Message; }
            }
            finally
            {
                Connected = false;
                try { _client?.Close(); } catch { }
                _client = null;
            }

            if (_run) Thread.Sleep(500);   /* 断了就重连 */
        }
    }

    static int IndexOfHeaderEnd(byte[] b)
    {
        for (int i = 0; i + 3 < b.Length; i++)
            if (b[i] == 13 && b[i + 1] == 10 && b[i + 2] == 13 && b[i + 3] == 10) return i;
        return -1;
    }

    void ExtractFrames(System.IO.MemoryStream acc)
    {
        var data = acc.ToArray();
        int pos = 0;

        while (true)
        {
            int soi = Find(data, pos, 0xFF, 0xD8);
            if (soi < 0) break;

            int eoi = Find(data, soi + 2, 0xFF, 0xD9);
            if (eoi < 0) break;

            SoiSeen++;
            int len = eoi + 2 - soi;
            if (len > 100)
            {
                var jpg = new byte[len];
                Buffer.BlockCopy(data, soi, jpg, 0, len);

                /* 只留最新一帧，避免堆积 */
                while (_queue.TryDequeue(out _)) { }
                _queue.Enqueue(jpg);
            }
            pos = eoi + 2;
        }

        if (pos > 0)
        {
            acc.SetLength(0);
            acc.Write(data, pos, data.Length - pos);
        }
        else if (data.Length > 4 * 1024 * 1024)
        {
            acc.SetLength(0);      /* 找不到边界就别无限涨 */
        }
    }

    static int Find(byte[] b, int from, byte b1, byte b2)
    {
        for (int i = from; i < b.Length - 1; i++)
            if (b[i] == b1 && b[i + 1] == b2) return i;
        return -1;
    }

    /// <summary>主线程调用：有帧就解码进 Texture</summary>
    public bool Pump()
    {
        if (_queue.IsEmpty) return false;

        if (_queue.TryDequeue(out var jpg))
        {
            try
            {
                if (Texture.LoadImage(jpg)) { FrameCount++; return true; }
                else LoadFail++;
            }
            catch (Exception e) { LastError = e.Message; LoadFail++; }
        }
        return false;
    }
}
