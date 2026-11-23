using System;
using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Networking;
using UnityEngine.XR;

/// <summary>
/// HT Turret 头显端 App（重写版）。
///
/// 设计原则：**只依赖已证明能工作的东西**
///   ✅ OpenXR 头姿（InputSystem 的 XRHMD）
///   ✅ BLE（Java 插件 BleLink）
///   ✅ Unlit/Texture 渲染一张贴图
///   ❌ 不用 UGUI / Canvas / TextMesh / Font / Collider / EventSystem / 物理射线
///
/// 界面全部画进一张 Texture2D（TextureUi），命中判定用「射线与面板平面求交」的纯数学。
/// </summary>
public class HeadSender : MonoBehaviour
{
    [Header("BLE")]
    public string deviceNamePrefix = "C5-";
    [Range(30, 120)] public int sendHz = 90;
    public bool logEverySecond = true;

    [Header("面板")]
    public float tiltDeg = 0f;
    public float distM   = 1.10f;
    public float widthM  = 1.30f;

    [Tooltip("透视（Passthrough）：需要清单声明 + AR Foundation + Camera 特性")]
    public bool passthrough = false;

    [Tooltip("面板是否跟着头走。关掉＝固定在空间里，转头是环视一块静止的屏幕")]
    public bool followHead = true;

    [Tooltip("跟随比例：1=完全跟随（贴死视野），0.5=只跟一半，0=不跟")]
    [Range(0f, 1f)] public float followK = 1f;

    [Tooltip("面板中心相对眼睛向下的距离（米）")]
    public float downOffset = 1.42f;

    [Tooltip("OnGUI 叠加（在 XR 下多数情况画不出来，默认关闭）")]
    public bool useOverlay = false;

    [Header("图传")]
    public bool videoOn = true;
    public string videoUrl = "http://192.168.4.2:8080/snapshot.jpg";
    public float videoFps = 12f;

    // ───────── 状态 ─────────
    float _yaw, _pitch, _yawRate, _lastYaw;
    int _seq;
    float _lastSend, _rateT0;
    int _sentBase;
    bool _ready;
    bool _pitchInvert;
    bool _yawInv = true;    /* 用户实测 yaw 方向反了，默认取反 */
    int _page;              /* 0=主页面 1=设置 2=图传源 */
    string _poseSrc = "-";
    int _stateCache;
    Vector3 _recenterEuler;

    // ───────── BLE ─────────
#if UNITY_ANDROID && !UNITY_EDITOR
    AndroidJavaObject _link;
    AndroidJavaClass  _cls;
    float _lastBeginTry;
#endif

    // ───────── 界面 ─────────
    TextureUi _ui;
    GameObject _panel;
    Renderer _videoQuad;          /* 图传专用 Quad（GPU 直接显示纹理） */
    Renderer _panelRend;
    Texture2D _backTex;
    bool _flipX, _flipY = true;
    float _redrawT;
    const float _redrawMinGap = 1f / 30f;   /* UI 最多 30 次/秒重绘 */
    bool _dirty = true;

    string _hoverId;
    float _gazeT;
    string _gazeId;
    bool _prevPressedL, _prevPressedR;
    bool _dumpedControls;
    string _lastPressedControl = "-";
    bool _usedPointer;
    float _beamLogT;
    bool _xrHandValid;
    Vector3 _xrHandPos;
    Vector3 _lastHeadPos;
    Vector3 _pointerPos;      /* 瞄准姿态的位置（暂不使用） */

    // ───────── 图传 ─────────
    /* 外接 UVC 采集卡（绿联）直接插 Quest 时走这条路 */
    WebCamTexture _webcam;
    Color32[] _lastCamPix;
    int _lastCamW, _lastCamH;
    int _mjpegSeen;
    Color32[] _camCachePix;
    int _mjpegSeenCache = -1;
    int _camCacheW, _camCacheH;
    Color32[] _camPix;
    string _camPick = "UGREEN";
    bool _useWebcam;
    int  _vsrc;            /* 0=关 1=USB采集卡 2=有线(adb reverse) 3=无线(WiFi) */
    string _pcIp = "192.168.4.2";   /* 无线时 PC 的 IP */
    int  _camIdx = -1;      /* >=0 时强制用这个下标的摄像头 */

    MjpegTcp _tcp;              /* 自建 Socket 读 MJPEG（UnityWebRequest 在 Quest 上会卡住） */
    MjpegStream _mjpeg;          /* 旧的 UnityWebRequest 实现，保留备用 */
    UnityWebRequest _streamReq;
    Texture2D _videoTex;
    bool _videoBusy;
    float _videoNext;
    int _videoOk, _videoFail;

    static readonly Color32 C_BG    = new Color32(12, 18, 28, 255);
    static readonly Color32 C_FACE  = new Color32(38, 50, 68, 255);
    static readonly Color32 C_EDGE  = new Color32(90, 120, 160, 255);
    static readonly Color32 C_TEXT  = new Color32(215, 230, 245, 255);
    static readonly Color32 C_GREEN = new Color32(90, 230, 140, 255);
    static readonly Color32 C_AMBER = new Color32(245, 190, 80, 255);
    static readonly Color32 C_VIDEO = new Color32(8, 10, 14, 255);

    // ────────────────────────────────────────────────────────────

    void Start()
    {
        LoadPrefs();
        LoadCfgFile();

        _videoTex = new Texture2D(2, 2, TextureFormat.RGBA32, false);
        _videoTex.SetPixels32(new[] { C_VIDEO, C_VIDEO, C_VIDEO, C_VIDEO });
        _videoTex.Apply();

        _ui = new TextureUi(1024, 512);
        _ui.FlipX = _flipX;
        _ui.FlipY = _flipY;
        BuildPanel();

#if UNITY_ANDROID && !UNITY_EDITOR
        using (var activity = new AndroidJavaClass("com.unity3d.player.UnityPlayer")
                   .GetStatic<AndroidJavaObject>("currentActivity"))
        {
            _cls  = new AndroidJavaClass("com.ht.turret.BleLink");
            _link = _cls.CallStatic<AndroidJavaObject>("create", activity);
            TryBegin();
        }
#endif
        DumpUsbDevices();
        ProbeUsbUvc();
        ApplyVideoSource();
        SetupPassthrough();
        Application.onBeforeRender += PlacePanel;
        _rateT0 = Time.realtimeSinceStartup;
        LogFile("[App] 启动：贴图界面 1024x512，BLE 前缀 " + deviceNamePrefix);
    }

    void OnDestroy()
    {
        Application.onBeforeRender -= PlacePanel;
#if UNITY_ANDROID && !UNITY_EDITOR
        _link?.Call("stop");
#endif
    }

    static Shader SShader()
    {
        return Shader.Find("Unlit/Texture") ?? Shader.Find("Standard");
    }

    /// <summary>
    /// 透视：给相机挂 ARCameraBackground（AR Foundation 组件），并清成透明背景。
    /// 用反射创建，AR Foundation 没装时不影响编译。
    /// </summary>
    /// <summary>请求 CAMERA 权限（Android 6+ 必须运行时申请）</summary>
    void RequestCameraPermission()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        try
        {
            using (var activity = new AndroidJavaClass("com.unity3d.player.UnityPlayer")
                       .GetStatic<AndroidJavaObject>("currentActivity"))
            using (var pm = activity.Call<AndroidJavaObject>("getPackageManager"))
            {
                int granted = activity.Call<int>("checkSelfPermission", "android.permission.CAMERA");
                if (granted != 0)   /* != PackageManager.PERMISSION_GRANTED */
                {
                    activity.Call("requestPermissions", new string[] { "android.permission.CAMERA" }, 9002);
                    LogFile("[App] 请求 CAMERA 权限");
                }
            }
        }
        catch (Exception e) { Debug.LogWarning("[App] 权限申请失败: " + e.Message); }
#endif
    }

    /// <summary>
    /// 逐个试摄像头：Quest 上会同时列出跟踪相机和 UVC 采集卡，
    /// 只有真正能出帧的那个才是我们想要的。
    /// </summary>
    /// <summary>
    /// 对同一个摄像头依次尝试多种分辨率/帧率。
    /// UVC 采集卡常常只支持特定模式（例如 1080p30 或 720p60），
    /// 请求到不支持的模式时表现就是一直没有帧。
    /// </summary>
    System.Collections.IEnumerator TryModes(string name)
    {
        int[,] modes = new int[,] {
            { 1920, 1080, 30 }, { 1920, 1080, 60 },
            { 1280, 720, 60 },  { 1280, 720, 30 },
            { 640, 480, 30 },
            { 0, 0, 0 },
        };

        for (int m = 0; m < modes.GetLength(0); m++)
        {
            int w = modes[m, 0], h = modes[m, 1], fps = modes[m, 2];
            WebCamTexture t = null;
            try
            {
                t = (w == 0) ? new WebCamTexture(name) : new WebCamTexture(name, w, h, fps);
                t.Play();
            }
            catch (Exception e)
            {
                LogFile("[App]   " + name + " " + w + "x" + h + " 打开失败: " + e.Message);
                continue;
            }

            float t0 = Time.realtimeSinceStartup;
            bool ok = false;
            while (Time.realtimeSinceStartup - t0 < 1.2f)
            {
                if (t.didUpdateThisFrame && t.width > 16) { ok = true; break; }
                yield return null;
            }

            LogFile("[App]   " + name + " " + w + "x" + h + "@" + fps + " -> " +
                    (ok ? "有帧 " + t.width + "x" + t.height : "无帧"));

            if (ok)
            {
                _webcam = t;
                _camPix = new Color32[t.width * t.height];
                LogFile("[App] 采用摄像头: " + name + " " + t.width + "x" + t.height);
                yield break;
            }

            t.Stop();
        }
    }

    System.Collections.IEnumerator ProbeCameras()
    {
        var devs = WebCamTexture.devices;
        LogFile("[App] 开始探测 " + devs.Length + " 个摄像头");

        for (int di = 0; di < devs.Length; di++)
        {
            if (_camIdx >= 0 && di != _camIdx) continue;
            var d = devs[di];
            LogFile("[App] 试 " + d.name);
            yield return TryModes(d.name);
            if (_webcam != null) yield break;
        }

        LogFile("[App] 所有摄像头都没有帧（UVC 需要原生方案）");
    }

    /// <summary>
    /// 枚举摄像头（Quest 上外接 UVC 采集卡会出现在 WebCamTexture.devices 里）。
    /// 只打日志 + 按名字挑一个，不做别的。
    /// </summary>
    /// <summary>
    /// 把当前 USB 设备列表写进 app.log：
    /// 采集卡的 VID/PID 和 class 都在这儿，写原生 UVC 时要靠它。
    /// </summary>
    /// <summary>
    /// 调 Java 的 UsbProbe：枚举 USB 设备、申请权限、打印接口/端点。
    /// 这是写原生 libuvc 之前的最后一块排查——先确认卡能不能被 open/claim。
    /// </summary>
    void ProbeUsbUvc()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        try
        {
            using (var activity = new AndroidJavaClass("com.unity3d.player.UnityPlayer")
                       .GetStatic<AndroidJavaObject>("currentActivity"))
            {
                var cls = new AndroidJavaClass("com.ht.turret.UsbProbe");
                cls.CallStatic("init", activity);
                cls.CallStatic("registerPermissionReceiver");
                cls.CallStatic("dump");
                LogFile("[App] UsbProbe 已调用");
            }
        }
        catch (Exception e) { LogFile("[App] UsbProbe 失败: " + e.Message); }
#endif
    }

    void DumpUsbDevices()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        try
        {
            using (var activity = new AndroidJavaClass("com.unity3d.player.UnityPlayer")
                       .GetStatic<AndroidJavaObject>("currentActivity"))
            using (var usb = activity.Call<AndroidJavaObject>("getSystemService", "usb"))
            {
                if (usb == null) { LogFile("[USB] 没有 usb 服务"); return; }

                var map = usb.Call<AndroidJavaObject>("getDeviceList");
                int n = map.Call<int>("size");
                LogFile("[USB] 设备数 " + n);

                var it = map.Call<AndroidJavaObject>("values").Call<AndroidJavaObject>("iterator");
                while (it.Call<bool>("hasNext"))
                {
                    var dev = it.Call<AndroidJavaObject>("next");
                    int vid = dev.Call<int>("getVendorId");
                    int pid = dev.Call<int>("getProductId");
                    int cls = dev.Call<int>("getDeviceClass");
                    string name = dev.Call<string>("getDeviceName");
                    LogFile("[USB] vid=0x" + vid.ToString("x4") + " pid=0x" + pid.ToString("x4") +
                            " class=" + cls + " " + name);
                }
            }
        }
        catch (Exception e) { LogFile("[USB] 枚举失败: " + e.Message); }
#endif
    }

    void SetupWebcam()
    {
        try
        {
            /* Android 上 WebCamTexture 需要 CAMERA 权限，没有就静默失败 */
            RequestCameraPermission();

            var devs = WebCamTexture.devices;
            var names = new List<string>();
            foreach (var d in devs) names.Add(d.name);
            LogFile("[App] 摄像头设备(" + devs.Length + "): " + string.Join(" | ", names));

            if (!_useWebcam || devs.Length == 0) return;

            StartCoroutine(ProbeCameras());
            return;

            string pick = null;
            foreach (var d in devs)
                if (d.name.ToLowerInvariant().Contains(_camPick.ToLowerInvariant())) { pick = d.name; break; }
            if (pick == null) pick = devs[0].name;

            _webcam = new WebCamTexture(pick, 640, 360, 30);
            _webcam.Play();
            _camPix = new Color32[640 * 360];
            LogFile("[App] 已启动摄像头: " + pick);
        }
        catch (Exception e)
        {
            Debug.LogWarning("[App] 摄像头枚举失败: " + e.Message);
        }
    }

    void SetupPassthrough()
    {
        var cam = Camera.main;
        if (cam == null) return;

        if (!passthrough)
        {
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = new Color(0.05f, 0.08f, 0.14f);
            return;
        }

        /* 背景透明，否则整片遮住现实 */
        cam.clearFlags = CameraClearFlags.SolidColor;
        cam.backgroundColor = new Color(0f, 0f, 0f, 0f);

        var t = Type.GetType("UnityEngine.XR.ARFoundation.ARCameraBackground, Unity.XR.ARFoundation");
        if (t == null)
        {
            Debug.LogWarning("[App] 没找到 ARCameraBackground（AR Foundation 未安装）");
            return;
        }

        /* ARCameraBackground 只负责把相机纹理画到背景，
           真正启动相机流还需要 ARCameraManager + ARSession */
        var tm = Type.GetType("UnityEngine.XR.ARFoundation.ARCameraManager, Unity.XR.ARFoundation");
        var ts = Type.GetType("UnityEngine.XR.ARFoundation.ARSession, Unity.XR.ARFoundation");

        if (tm != null && cam.GetComponent(tm) == null)
        {
            cam.gameObject.AddComponent(tm);
            Debug.Log("[App] 已挂 ARCameraManager");
        }

        if (ts != null && GameObject.Find("ARSession") == null)
        {
            var sess = new GameObject("ARSession");
            sess.AddComponent(ts);
            Debug.Log("[App] 已创建 ARSession");
        }

        if (cam.GetComponent(t) != null) return;

        try
        {
            cam.gameObject.AddComponent(t);
            Debug.Log("[App] 已挂 ARCameraBackground，透视背景生效");
        }
        catch (Exception e)
        {
            Debug.LogWarning("[App] 挂 ARCameraBackground 失败: " + e.Message);
        }
    }
    /// <summary>图传专用 Quad：挂在面板下，尺寸对应 UI 里那块 640x360 区域</summary>
    void BuildVideoQuad()
    {
        var q = GameObject.CreatePrimitive(PrimitiveType.Quad);
        q.name = "VideoQuad";
        q.transform.SetParent(_panel.transform, false);
        Destroy(q.GetComponent<Collider>());

        /* UI 贴图 1024x512，图传占 640x360 居中：
           宽度比 640/1024 = 0.625，高度比 360/512 = 0.703 */
        q.transform.localScale = new Vector3(640f / 1024f, 360f / 512f, 1f);
        q.transform.localPosition = new Vector3(0f, 0f, -0.02f);

        var r = q.GetComponent<Renderer>();
        var sh = Shader.Find("Unlit/Texture") ?? Shader.Find("Standard");
        r.material = new Material(sh);
        r.material.color = Color.white;
        r.enabled = false;
        _videoQuad = r;
    }

    void BuildPanel()
    {
        _panel = GameObject.CreatePrimitive(PrimitiveType.Quad);
        _panel.name = "UiPanel";

        _panel.transform.SetParent(null, false);

        Destroy(_panel.GetComponent<Collider>());

        _panelRend = _panel.GetComponent<Renderer>();
        _panelRend.material = new Material(SShader());
        _panelRend.material.mainTexture = _ui.Tex;
        _panelRend.material.color = Color.white;

        BuildVideoQuad();   /* 图传专用 Quad（GPU 显示） */

        /* 3D 面板已弃用：改用 OnGUI 直接画到眼睛画面里，
           避免世界坐标/朝向/镜像这类问题。Quad 只留作参考，直接禁用。 */
        /* 3D 面板重新启用（OnGUI 在 XR 里画不出来） */

        /* 背面板：Quad 只朝一面，挂一块转 180° 的，彻底不用猜朝向 */
        var back = GameObject.CreatePrimitive(PrimitiveType.Quad);
        back.name = "UiPanelBack";
        back.transform.SetParent(_panel.transform, false);
        back.transform.localRotation = Quaternion.Euler(0f, 180f, 0f);
        back.transform.localPosition = Vector3.zero;
        back.transform.localScale = Vector3.one;
        Destroy(back.GetComponent<Collider>());
        var br = back.GetComponent<Renderer>();
        br.material = new Material(SShader());
        _backTex = new Texture2D(1024, 512, TextureFormat.RGBA32, false);
        _backTex.wrapMode = TextureWrapMode.Clamp;
        _backTex.filterMode = FilterMode.Point;
        br.material.mainTexture = _backTex;
    }

    // ─────────────────────────── 文件日志 ───────────────────────────

    string LogPath
    {
        get { return System.IO.Path.Combine(Application.persistentDataPath, "app.log"); }
    }

    /// <summary>
    /// 同时写进 Unity 控制台和 app.log。
    /// 采集卡占着唯一的 USB 口时 adb 用不了，靠这个文件事后取证。
    /// </summary>
    public void LogFile(string msg)
    {
        Debug.Log(msg);
        try
        {
            var fi = new System.IO.FileInfo(LogPath);
            if (fi.Exists && fi.Length > 256 * 1024) System.IO.File.Delete(LogPath);
            System.IO.File.AppendAllText(LogPath,
                DateTime.Now.ToString("HH:mm:ss.fff") + "  " + msg + "\n");
        }
        catch { }
    }

    // ─────────────────────────── 主循环 ───────────────────────────

    void Update()
    {
        if (_tcp != null) _tcp.Pump();   /* 主线程解码 TCP 收到的 JPEG */

        if (_videoQuad != null)
        {
            bool show = _tcp != null && _tcp.FrameCount > 0 && videoOn;
            if (_videoQuad.enabled != show) _videoQuad.enabled = show;
            if (show && _videoQuad.material.mainTexture != _tcp.Texture)
                _videoQuad.material.mainTexture = _tcp.Texture;
        }

        if (TryGetHeadPose(out Quaternion q, out _))
        {
            Vector3 fwd = q * Vector3.forward;
            _yaw   = Mathf.Atan2(-fwd.x, -fwd.z) * Mathf.Rad2Deg;
            _pitch = Mathf.Asin(Mathf.Clamp(fwd.y, -1f, 1f)) * Mathf.Rad2Deg;
            if (_pitchInvert) _pitch = -_pitch;
            _yawCont += Mathf.DeltaAngle(_yawPrev, _yaw);   /* 平滑累计，不绕圈 */
            _yawPrev = _yaw;
            _yawRate = Mathf.DeltaAngle(_lastYaw, _yaw) / Mathf.Max(Time.deltaTime, 1e-4f);
            _lastYaw = _yaw;
        }

        HandleInput();

        if (_aimValid && Time.realtimeSinceStartup - _redrawT > 0.05f &&
            Mathf.Abs(_aimPx - _lastAimPx) + Mathf.Abs(_aimPy - _lastAimPy) > 6f)
        {
            _lastAimPx = _aimPx; _lastAimPy = _aimPy;
            _dirty = true;
        }

        if (_webcam != null && _webcam.didUpdateThisFrame) _dirty = true;
        if (_mjpeg != null && _mjpeg.FrameCount != _mjpegSeen) { _mjpegSeen = _mjpeg.FrameCount; _dirty = true; }

        if ((Time.realtimeSinceStartup - _redrawT >= 0.25f || _dirty) &&
            Time.realtimeSinceStartup - _redrawT >= _redrawMinGap)
        {
            _redrawT = Time.realtimeSinceStartup;
            _dirty = false;
            Redraw();
        }

        SendPose();
        MaybeGrabVideo();

        if (logEverySecond && Time.realtimeSinceStartup - _rateT0 >= 1f)
        {
            _rateT0 = Time.realtimeSinceStartup;
#if UNITY_ANDROID && !UNITY_EDITOR
            int sent = _link != null ? _link.Call<int>("getSentCount") : 0;
            _stateCache = _link != null ? _link.Call<int>("getState") : 0;
            _ready = _link != null && _link.Call<bool>("isReady");
            string err = _link != null ? _link.Call<string>("getError") : "";
            if (_tcp != null)
                LogFile("[App] MJPEG(TCP) connected=" + _tcp.Connected +
                        " frames=" + _tcp.FrameCount + " bytes=" + _tcp.TotalBytes +
                        " err=" + _tcp.LastError + " soi=" + _tcp.SoiSeen + " loadFail=" + _tcp.LoadFail + " hdr=" + _tcp.HeaderHex);

            LogFile("[App] yaw " + _yaw.ToString("+0.0;-0.0") + " pitch " + _pitch.ToString("+0.0;-0.0") +
                      " ready " + _ready + " state " + _stateCache + " sent/s " + (sent - _sentBase) +
                      " pose " + _poseSrc + " video " + _videoOk + "/" + _videoFail + " " + err);
            _sentBase = sent;
#else
            Debug.Log("[App] (编辑器) yaw " + _yaw.ToString("+0.0;-0.0") +
                      " pitch " + _pitch.ToString("+0.0;-0.0") + " pose " + _poseSrc);
#endif
        }
    }

    /// <summary>
    /// 面板摆放放到 LateUpdate：XR 的 HMD 位姿在帧末才最终确定，
    /// 在 Update 里用它会让面板晚一帧跟上，转头快时表现为"冲出去/跟不上"。
    /// </summary>
    /* 注意：这里不再用 LateUpdate。
       Unity 的 XR 渲染姿态在"提交帧"那一刻才锁定（带预测），
       LateUpdate 里摆面板仍会晚一步；Application.onBeforeRender 是官方推荐的钩子。 */

    void SendPose()
    {
        if (Time.realtimeSinceStartup - _lastSend < 1f / Mathf.Max(1, sendHz)) return;
        _lastSend = Time.realtimeSinceStartup;
        _seq++;

#if UNITY_ANDROID && !UNITY_EDITOR
        if (_link == null) return;

        if (!_ready && Time.realtimeSinceStartup - _lastBeginTry > 2f) TryBegin();

        if (_ready)
        {
            _link.Call<bool>("send",
                (short)Mathf.RoundToInt((_yawInv ? -_yaw : _yaw) * 100f),
                (short)Mathf.RoundToInt(_pitch * 100f),
                (short)Mathf.RoundToInt(_yawRate * 10f),
                _seq, 0x01);
        }
#endif
    }

#if UNITY_ANDROID && !UNITY_EDITOR
    void TryBegin()
    {
        if (_link == null) return;
        _lastBeginTry = Time.realtimeSinceStartup;
        bool ok = _link.Call<bool>("begin", deviceNamePrefix);
        Debug.Log("[App] BLE begin=" + ok + " state=" + _link.Call<int>("getState") +
                  " " + _link.Call<string>("getError"));
    }
#endif

    // ─────────────────────────── 面板摆放 ───────────────────────────

    float _placeLogT;

    bool _placedOnce;
    bool _yawBaseSet;
    Vector3 _yawBase = Vector3.forward;

    /// <summary>手柄光柱：从手柄射到瞄准点，让"指哪儿"看得见</summary>
    LineRenderer[] _beams = new LineRenderer[2];

    void EnsureBeams()
    {
        for (int i = 0; i < 2; i++)
        {
            if (_beams[i] != null) continue;

            var go = new GameObject("AimBeam" + i);
            var lr = go.AddComponent<LineRenderer>();
            lr.positionCount = 2;
            lr.startWidth = 0.012f;
            lr.endWidth = 0.004f;
            lr.useWorldSpace = true;
            lr.material = new Material(Shader.Find("Unlit/Color") ?? Shader.Find("Standard"));
            lr.material.color = new Color(0.35f, 1f, 0.6f, 0.85f);
            lr.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            lr.receiveShadows = false;
            _beams[i] = lr;
        }
    }

    void UpdateBeam(int i, bool valid, Vector3 from, Vector3 dir, Vector3 end)
    {
        if (_beams[i] == null) return;
        _beams[i].enabled = valid;
        if (!valid) return;
        _beams[i].SetPosition(0, from);
        _beams[i].SetPosition(1, end);
    }

    static string Fmt(Vector3 v)
    {
        return "(" + v.x.ToString("0.00") + "," + v.y.ToString("0.00") + "," +
               v.z.ToString("0.00") + ")";
    }

    /// <summary>从 UnityEngine.XR 拿手柄位置（比 Input System 的 devicePosition 更可靠）</summary>
    bool TryGetHandPosXR(bool left, out Vector3 pos)
    {
        pos = Vector3.zero;
        var node = left ? UnityEngine.XR.XRNode.LeftHand : UnityEngine.XR.XRNode.RightHand;
        var dev = UnityEngine.XR.InputDevices.GetDeviceAtXRNode(node);
        _xrHandValid = dev.isValid;
        if (!dev.isValid) return false;
        bool ok = dev.TryGetFeatureValue(UnityEngine.XR.CommonUsages.devicePosition, out pos);
        _xrHandPos = pos;
        return ok;
    }

    void PlacePanel()
    {
        if (_panel == null) return;
        if (!followHead && _placedOnce) return;          /* 固定模式：只摆一次 */
        if (!TryGetHeadPose(out Quaternion q, out Vector3 hp)) return;
        _lastHeadPos = hp;
        _placedOnce = true;

        /* 水平跟随用"头部前向的水平投影"直接算，不再用 eulerAngles.y。
           euler 在某些四元数下会给出非线性的 yaw，表现为"左转时面板转得比头多、
           45° 就转出视野"。用向量分量算，角度是多少就走多少。 */
        Vector3 fwd = q * Vector3.forward;
        fwd.y = 0f;
        if (fwd.sqrMagnitude < 1e-4f) fwd = Vector3.forward;
        fwd.Normalize();

        /* followK < 1 时，面板只跟一部分转角：
           先把当前水平朝向存成基准，再按比例插值，转头越多偏出越多。 */
        if (!_yawBaseSet)
        {
            _yawBaseSet = true;
            _yawBase = fwd;
            _yawBaseYaw = Quaternion.LookRotation(fwd, Vector3.up).eulerAngles.y;
            _yawBaseCont = _yawCont;      /* 记住基准时刻的累计角 */
        }

        if (followK < 0.999f)
        {
            /* 用累计角差，跨 ±180 也不会绕整圈 */
            float d = _yawCont - _yawBaseCont;
            fwd = Quaternion.Euler(0f, _yawBaseYaw + d * followK, 0f) * Vector3.forward;
        }
        if (fwd.sqrMagnitude < 1e-4f) fwd = Vector3.forward;   /* 抬头到垂直时的兜底 */
        fwd.Normalize();

        Vector3 pos = hp + fwd * distM + Vector3.down * downOffset;
        _panel.transform.position = pos;

        /* 朝向眼睛 + 结果再按 tilt 上仰，保证正面朝你 */
        Vector3 toEye = hp - pos;
        toEye.y = 0f;
        if (toEye.sqrMagnitude < 1e-4f) toEye = -fwd;
        _panel.transform.rotation =
            Quaternion.LookRotation(toEye.normalized, Vector3.up) * Quaternion.Euler(tiltDeg, 180f, 0f);

        _panel.transform.localScale = new Vector3(widthM, widthM * 0.5f, 1f);

        if (Time.realtimeSinceStartup - _placeLogT > 5f)
        {
            _placeLogT = Time.realtimeSinceStartup;
            var p = _panel.transform.position;
            Vector3 d = p - hp; d.y = 0f;
            float dev = Vector3.Angle(fwd, d.normalized);      /* 面板偏离视线正前方多少度 */
            Debug.Log("[App] 面板偏离视线 " + dev.ToString("0.0") + "°  down=" + downOffset.ToString("0.00") +
                      " 面板y=" + p.y.ToString("0.00") + " 头y=" + hp.y.ToString("0.00"));
        }
    }

    // ─────────────────────────── 输入与命中 ───────────────────────────

    string _lastHover;

    void HandleInput()
    {
        _hoverId = null;
        _aimValid = false;

        Quaternion headRot = Quaternion.identity;
        if (!TryGetHeadPose(out headRot, out _)) return;   /* 没有头姿就不做命中 */

        /* ① 手柄：左右手各一条射线 */
        for (int i = 0; i < 2; i++)
        {
            bool left = i == 0;
            if (!TryGetHand(left, out Vector3 pos, out Quaternion rot, out bool pressed)) continue;

            Vector3 dirW = rot * Vector3.forward;
            string id = PickByAim(headRot, dirW);
            bool prev = left ? _prevPressedL : _prevPressedR;

            /* 光柱：从手柄射到面板平面（打不到就 3 米处） */
            {
                int bi = left ? 0 : 1;
                Vector3 origin = pos;
                if (TryGetHandPosXR(left, out Vector3 xrPos)) origin = xrPos;
                Vector3 end = origin + dirW * 3f;
                if (_panel != null)
                {
                    var plane = new Plane(_panel.transform.forward, _panel.transform.position);
                    if (plane.Raycast(new Ray(origin, dirW), out float enter) && enter > 0f && enter < 8f)
                        end = origin + dirW * enter;
                }
                /* 光柱已按用户要求移除 */

                if (Time.realtimeSinceStartup - _beamLogT > 3f)
                {
                    _beamLogT = Time.realtimeSinceStartup;
                    LogFile("[App] 光柱诊断 " + (left ? "L" : "R") +
                            " xrValid=" + _xrHandValid +
                            " xrPos=" + Fmt(_xrHandPos) +
                            " isPos=" + Fmt(pos) +
                            " dir=" + Fmt(dirW) +
                            " 头=" + Fmt(_lastHeadPos));
                }
            }

            /* A/X 键：把面板重新摆到当前朝向前方（修正初始偏位） */
            if (_primaryDown && !_prevPrimary)
            {
                LogFile("[App] A/X 回中");
                Press("RECENTER");
            }
            _prevPrimary = _primaryDown;

            if (pressed && !prev)
            {
                Debug.Log("[App] 扳机 " + (left ? "左" : "右") +
                          " 瞄准=(" + _aimPx.ToString("0") + "," + _aimPy.ToString("0") + ")" +
                          " 命中=" + (id ?? "无") +
                          " 控件=" + _lastPressedControl + " pointer=" + _usedPointer);
            }

            if (id != null)
            {
                _hoverId = id;
                if (pressed && !prev) Press(id);
            }

            if (left) _prevPressedL = pressed; else _prevPressedR = pressed;
        }

        if (_hoverId != _lastHover)
        {
            _lastHover = _hoverId;
            _dirty = true;
        }

        /* ② 视线停留 1.2 秒（手柄按不动时的兜底） */
        if (TryGetHeadPose(out Quaternion gazeRot, out _))
        {
            string id = PickByAim(gazeRot, gazeRot * Vector3.forward);
            if (id == null) { _gazeId = null; _gazeT = 0f; }
            else if (id != _gazeId) { _gazeId = id; _gazeT = 0f; }
            else
            {
                _gazeT += Time.deltaTime;
                if (_gazeT >= 1.2f) { _gazeT = 0f; Press(id); }
            }
        }
    }

    /* 虚拟视场：水平 100°，垂直按 16:9 推算。
       GUI 画出来的区域和这个视场一一对应，所以角度 → 像素是线性的。 */
    const float FovH = 100f;

    /// <summary>
    /// 把手柄（或视线）相对头部的方向换算成屏幕像素，再查按钮。
    /// 完全不碰世界坐标、不碰 Quad、不碰镜像。
    /// </summary>
    string PickByAim(Quaternion headRot, Vector3 dir)
    {
        Vector3 local = Quaternion.Inverse(headRot) * dir;   // 头局部坐标：+Z 前方
        if (local.z <= 0.05f) return null;                   // 指向身后

        float tanH = Mathf.Tan(FovH * 0.5f * Mathf.Deg2Rad);
        float tanV = tanH * _ui.H / (float)_ui.W;

        float ndcX = (local.x / local.z) / tanH;             // -1 ~ 1
        float ndcY = (local.y / local.z) / tanV;

        float px = (ndcX * 0.5f + 0.5f) * _ui.W;
        float py = (1f - (ndcY * 0.5f + 0.5f)) * _ui.H;      // 屏幕 y 向下

        _aimPx = px;
        _aimPy = py;
        _aimValid = true;
        _lastAimId = _ui.Pick(px, py);

        return _ui.Pick(px, py);
    }

    float _aimPx, _aimPy;
    string _lastAimId;
    bool _prevPrimary;
    bool _primaryDown;
    float _lastAimPx, _lastAimPy;
    float _yawCont;        /* 连续累计的偏航角，跨 ±180 不会跳 */
    float _yawPrev;
    float _yawBaseYaw;    /* 基准朝向的偏航角 */
    float _yawBaseCont;   /* 基准时刻的累计角 */
    bool _aimValid;

    void Press(string id)
    {
        LogFile("[App] 点击 " + id);

        switch (id)
        {
            case "SET":      _page = 1;  break;
            case "BACK":     _page = 0; break;
            case "TILT-":    tiltDeg = Mathf.Clamp(tiltDeg - 5f, -20f, 60f); break;
            case "TILT+":    tiltDeg = Mathf.Clamp(tiltDeg + 5f, -20f, 60f); break;
            case "DIST-":    distM   = Mathf.Clamp(distM - 0.1f, 0.6f, 3f); break;
            case "DIST+":    distM   = Mathf.Clamp(distM + 0.1f, 0.6f, 3f); break;
            case "WID-":     widthM  = Mathf.Clamp(widthM - 0.1f, 0.6f, 2.4f); break;
            case "WID+":     widthM  = Mathf.Clamp(widthM + 0.1f, 0.6f, 2.4f); break;
            case "DOWN-":    downOffset = Mathf.Clamp(downOffset - 0.05f, -0.20f, 1.20f); break;
            case "DOWN+":    downOffset = Mathf.Clamp(downOffset + 0.05f, -0.20f, 1.20f); break;
            case "HZ-":      sendHz  = Mathf.Clamp(sendHz - 15, 30, 120); break;
            case "HZ+":      sendHz  = Mathf.Clamp(sendHz + 15, 30, 120); break;
            case "INV":      _pitchInvert = !_pitchInvert; break;
            case "SRC":      _vsrc = (_vsrc + 1) % 4; ApplyVideoSource(); break;
            case "VIDSRC":   _page = 2; break;
            case "VBACK":    _page = 1; break;
            case "SRC0":     _vsrc = 0; ApplyVideoSource(); break;
            case "SRC1":     _vsrc = 1; ApplyVideoSource(); break;
            case "SRC2":     _vsrc = 2; ApplyVideoSource(); break;
            case "SRC3":     _vsrc = 3; ApplyVideoSource(); break;
            case "RECENTER": _recenterEuler = new Vector3(0f, _yaw, 0f); _yawBaseSet = false; _yawBaseCont = _yawCont; break;
            case "RECONN":   Reconnect(); break;
            case "VIDEO":    videoOn = !videoOn; break;
        }

        SavePrefs();
        _dirty = true;
    }

    void Reconnect()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        _ready = false;
        _lastBeginTry = 0f;
#endif
    }

    // ─────────────────────────── 绘制 ───────────────────────────

    void Redraw()
    {
        _ui.Clear(C_BG);
        _ui.ClearButtons();

        /* 图传居中：640×360 放在 1024×512 的正中 */
        var videoRect = new RectInt((_ui.W - 640) / 2, (_ui.H - 360) / 2, 640, 360);
        _ui.Fill(videoRect, C_VIDEO);
        _ui.Border(videoRect, C_EDGE, 2);
        /* 图传由 VideoQuad 直接显示，这里只画边框 */

        /* HUD 叠在图传上方，半透明底色保证可读 */
        _ui.Fill(new RectInt(16, 12, 420, 108), new Color32(0, 0, 0, 170));
        _ui.Text("HT TURRET", 24, 20, 4, C_TEXT);
        _ui.Text(_ready ? "BLE LINKED" : "BLE WAIT", 24, 60, 3,
                 _ready ? C_GREEN : C_AMBER);
        _ui.Text("YAW " + _yaw.ToString("+0;-0") + "o  PIT " + _pitch.ToString("+0;-0") + "o",
                 24, 92, 2, C_TEXT);

        /* 右下角：状态 + 按钮 */
        _ui.Text(sendHz + "HZ " + _stateCache + "  VID " + _videoOk, 700, 396, 2, C_TEXT);

        if (_page == 0)
        {
            _ui.Button("SET", new RectInt(700, 420, 150, 56), "SETUP", 3, C_FACE, C_EDGE, C_TEXT,
                       _hoverId == "SET");
            _ui.Button("VIDEO", new RectInt(862, 420, 146, 56), videoOn ? "VID ON" : "VID OFF", 2,
                       C_FACE, C_EDGE, C_TEXT, _hoverId == "VIDEO");
        }
        else if (_page == 1)
        {
            DrawSettings();
        }
        else
        {
            DrawVideoPage();
        }

        /* 准星：让人看得见手柄指到哪儿 */
        if (_aimValid)
        {
            int cx = Mathf.Clamp((int)_aimPx, 8, _ui.W - 9);
            int cy = Mathf.Clamp((int)_aimPy, 8, _ui.H - 9);

            var hot = _hoverId != null;
            var col = hot ? new Color32(90, 255, 140, 255) : new Color32(255, 220, 70, 255);

            _ui.Fill(new RectInt(cx - 10, cy - 1, 20, 3), col);
            _ui.Fill(new RectInt(cx - 1, cy - 10, 3, 20), col);
        }

        _ui.Apply();
    }

    void DrawSettings()
    {
        _ui.Fill(new RectInt(640, 8, 380, 500), new Color32(0, 0, 0, 210));

        int y = 16;
        const int rowH = 46;

        _ui.Text("SETTINGS", 656, y, 3, C_TEXT);
        y += 42;

        Row("TILT",  ref y, rowH, "TILT-", "TILT+", tiltDeg.ToString("0") + "o");
        Row("DIST",  ref y, rowH, "DIST-", "DIST+", distM.ToString("0.0") + "M");
        Row("WIDTH", ref y, rowH, "WID-",  "WID+",  widthM.ToString("0.0") + "M");
        Row("DOWN",  ref y, rowH, "DOWN-", "DOWN+", downOffset.ToString("0.00"));
        Row("RATE",  ref y, rowH, "HZ-",   "HZ+",   sendHz + "HZ");

        _ui.Button("SRC", new RectInt(656, y, 150, rowH - 8), "SRC " + SrcName(), 2,
                   C_FACE, C_EDGE, C_GREEN, _hoverId == "SRC");
        _ui.Button("INV", new RectInt(818, y, 130, rowH - 8), "PITCH INV", 2,
                   C_FACE, C_EDGE, _pitchInvert ? C_GREEN : C_TEXT, _hoverId == "INV");
        _ui.Button("RECONN", new RectInt(818, y, 130, rowH - 8), "RECONN", 2,
                   C_FACE, C_EDGE, C_TEXT, _hoverId == "RECONN");
        y += rowH;

        _ui.Button("VIDSRC", new RectInt(656, y, 292, rowH - 4), "VIDEO SOURCE...", 2,
                   C_FACE, C_EDGE, C_GREEN, _hoverId == "VIDSRC");
        y += (int)(rowH * 1.1f);
        _ui.Button("BACK", new RectInt(656, y, 292, rowH - 4), "BACK", 3,
                   new Color32(90, 46, 46, 255), C_EDGE, C_TEXT, _hoverId == "BACK");
    }

    /// <summary>图传源选择页：关 / USB / 有线 / 无线</summary>
    void DrawVideoPage()
    {
        _ui.Fill(new RectInt(120, 40, 784, 432), new Color32(0, 0, 0, 225));
        _ui.Border(new RectInt(120, 40, 784, 432), C_EDGE, 2);

        _ui.Text("VIDEO SOURCE", 160, 70, 4, C_TEXT);
        _ui.Text("current: " + SrcName(), 160, 120, 3, C_GREEN);

        int y = 180;
        const int w = 700, h = 64;

        Pick("SRC0", "OFF", 160, y, w, h, _vsrc == 0); y += 78;
        Pick("SRC1", "USB CAPTURE (blocked by Horizon OS)", 160, y, w, h, _vsrc == 1); y += 78;
        Pick("SRC2", "WIRED  (adb reverse 127.0.0.1:8080)", 160, y, w, h, _vsrc == 2); y += 78;
        Pick("SRC3", "WIRELESS  " + _pcIp + ":8080", 160, y, w, h, _vsrc == 3); y += 78;

        _ui.Button("VBACK", new RectInt(160, y, 220, 52), "BACK", 3,
                   new Color32(90, 46, 46, 255), C_EDGE, C_TEXT, _hoverId == "VBACK");
    }

    void Pick(string id, string label, int x, int y, int w, int h, bool selected)
    {
        var face = selected ? new Color32(40, 110, 70, 255) : C_FACE;
        var col = selected ? C_GREEN : C_TEXT;
        _ui.Button(id, new RectInt(x, y, w, h), (selected ? "[x] " : "[ ] ") + label, 2,
                   face, C_EDGE, col, _hoverId == id);
    }

    void Row(string label, ref int y, int rowH, string minus, string plus, string value)
    {
        _ui.Text(label, 656, y + 12, 2, C_TEXT);
        _ui.Button(minus, new RectInt(760, y, 52, rowH - 8), "-", 3, C_FACE, C_EDGE, C_TEXT,
                   _hoverId == minus);
        _ui.Button(plus, new RectInt(818, y, 52, rowH - 8), "+", 3, C_FACE, C_EDGE, C_TEXT,
                   _hoverId == plus);
        _ui.Text(value, 878, y + 10, 2, C_GREEN);
        y += rowH;
    }

    // ─────────────────────────── 屏幕叠加绘制 ───────────────────────────

    /// <summary>
    /// 直接画到眼睛画面里。
    /// VR 里 OnGUI 会把内容叠到双眼缓冲区，位置/朝向完全由屏幕坐标决定，
    /// 不再受世界坐标、Quad 朝向、背面剔除、左右手坐标系影响。
    /// </summary>
    void OnGUI()
    {
        if (_ui == null || !useOverlay) return;

        float h = Screen.height * 0.62f;                 /* 占屏高 62% */
        float w = h * _ui.W / _ui.H;
        if (w > Screen.width * 0.92f) { w = Screen.width * 0.92f; h = w * _ui.H / _ui.W; }

        float x = (Screen.width - w) * 0.5f;
        float y = Screen.height * 0.16f;                 /* 略偏上，留出下方空间 */

        GUI.color = Color.white;
        GUI.DrawTexture(new Rect(x, y, w, h), _ui.Tex, ScaleMode.StretchToFill);

        /* 瞄准点：手柄/视线当前指向的屏幕位置 */
        if (_aimValid)
        {
            float cx = x + _aimPx / _ui.W * w;
            float cy = y + _aimPy / _ui.H * h;
            var old = GUI.color;
            GUI.color = new Color(1f, 0.9f, 0.3f, 0.9f);
            GUI.DrawTexture(new Rect(cx - 6f, cy - 6f, 12f, 12f), Texture2D.whiteTexture);
            GUI.color = old;
        }
    }

    // ─────────────────────────── 图传 ───────────────────────────

    /// <summary>按 _vsrc 切换图传源（配置或设置页都能改）</summary>
    void ApplyVideoSource()
    {
        /* 先停掉旧的 */
        if (_streamReq != null) { try { _streamReq.Abort(); _streamReq.Dispose(); } catch { } _streamReq = null; }
        if (_tcp != null) { try { _tcp.StopStream(); } catch { } }
        _mjpeg = null;
        if (_webcam != null) { try { _webcam.Stop(); } catch { } _webcam = null; }

        switch (_vsrc)
        {
            case 0:
                videoOn = false;
                LogFile("[App] 图传源: 关");
                return;

            case 1:
                videoOn = true;
                _useWebcam = true;
                SetupWebcam();
                LogFile("[App] 图传源: USB 采集卡");
                return;

            case 2:
                videoOn = true;
                _useWebcam = false;
                videoUrl = "http://127.0.0.1:8080/stream.mjpg";
                StartMjpegIfNeeded();
                LogFile("[App] 图传源: 有线(adb reverse)");
                return;

            case 3:
                videoOn = true;
                _useWebcam = false;
                videoUrl = "http://" + _pcIp + ":8080/stream.mjpg";
                StartMjpegIfNeeded();
                LogFile("[App] 图传源: 无线 " + _pcIp);
                return;
        }
    }

    string SrcName()
    {
        switch (_vsrc)
        {
            case 0: return "OFF";
            case 1: return "USB";
            case 2: return "WIRED";
            case 3: return "WIFI";
            default: return "?";
        }
    }

    /// <summary>http 模式：开一条 MJPEG 长连接，持续收帧</summary>
    void StartMjpegIfNeeded()
    {
        if (_useWebcam) return;

        try
        {
            if (_tcp == null) _tcp = gameObject.AddComponent<MjpegTcp>();

            var uri = new Uri(videoUrl);
            _tcp.host = uri.Host;
            _tcp.port = uri.Port;
            _tcp.path = uri.AbsolutePath;
            _tcp.StartStream();

            LogFile("[App] MJPEG(TCP) 连接: " + _tcp.host + ":" + _tcp.port + _tcp.path);
        }
        catch (Exception e) { LogFile("[App] MJPEG 启动失败: " + e.Message); }
    }

    /// <summary>盯着 MJPEG 请求：出错误就记下来，帧数变化也记一次</summary>
    System.Collections.IEnumerator WatchMjpeg(string url)
    {
        float t0 = Time.realtimeSinceStartup;
        int lastSeen = 0;

        while (_streamReq != null)
        {
            yield return new WaitForSeconds(2f);

            if (_streamReq == null) break;

            string state = "running";
            if (_streamReq.isNetworkError || _streamReq.isHttpError) state = "错误";
            else if (_streamReq.result != UnityWebRequest.Result.InProgress) state = "已结束";

            int frames = _mjpeg != null ? _mjpeg.FrameCount : 0;

            long bytes = _mjpeg != null ? _mjpeg.TotalBytes : 0;
            LogFile("[App] MJPEG(TCP) connected=" + (_tcp != null && _tcp.Connected) +
                    " frames=" + (_tcp != null ? _tcp.FrameCount : 0) +
                    " bytes=" + (_tcp != null ? _tcp.TotalBytes : 0) +
                    " err=" + (_tcp != null ? _tcp.LastError : "-") +
                    " code=" + 0 +
                    " frames=" + frames + " bytes=" + bytes +
                    " err=" + (_streamReq.error ?? "-"));
            lastSeen = frames;

            if (state != "running") break;
        }
    }

    void MaybeGrabVideo()
    {
        if (!videoOn) return;
        if (_webcam != null) return;          /* 摄像头模式不抓 HTTP */
        if (_streamReq != null) return;       /* 旧 MJPEG 长连接 */
        if (_tcp != null && _tcp.Connected) return;   /* TCP 流在跑，不再轮询快照 */
        if (_videoBusy) return;
        if (Time.realtimeSinceStartup < _videoNext) return;
        _videoNext = Time.realtimeSinceStartup + 1f / Mathf.Max(1f, videoFps);
        StartCoroutine(GrabVideo());
    }

    IEnumerator GrabVideo()
    {
        _videoBusy = true;
        using (var req = UnityWebRequest.Get(videoUrl))
        {
            req.timeout = 2;
            yield return req.SendWebRequest();

            if (req.result == UnityWebRequest.Result.Success &&
                req.downloadHandler != null && req.downloadHandler.data != null &&
                _videoTex.LoadImage(req.downloadHandler.data))
            {
                _videoOk++;
                _dirty = true;
            }
            else
            {
                _videoFail++;
            }
        }
        _videoBusy = false;
    }

    /// <summary>把图传画面贴进 UI 像素缓冲：优先外接采集卡，其次 HTTP 快照</summary>
    void BlitVideo(RectInt dst)
    {
        if (_tcp != null && _tcp.FrameCount > 0)
        {
            if (_mjpegSeenCache != _tcp.FrameCount || _camCachePix == null)
            {
                _mjpegSeenCache = _tcp.FrameCount;
                _camCachePix = _tcp.Texture.GetPixels32();
                _camCacheW = _tcp.Texture.width;
                _camCacheH = _tcp.Texture.height;
            }
            if (_camCachePix != null) _ui.Blit(dst, _camCachePix, _camCacheW, _camCacheH);
            return;
        }

        if (_mjpeg != null && _mjpeg.FrameCount > 0)
        {
            /* 拿到新帧才取像素：GetPixels32 在 1080p 上每次要拷 8MB，
               每帧都调会把主线程拖死（表现为整体卡顿） */
            if (_mjpegSeenCache != _mjpeg.FrameCount || _camCachePix == null)
            {
                _mjpegSeenCache = _mjpeg.FrameCount;
                _camCachePix = _mjpeg.Texture.GetPixels32();
                _camCacheW = _mjpeg.Texture.width;
                _camCacheH = _mjpeg.Texture.height;
            }
            if (_camCachePix != null) _ui.Blit(dst, _camCachePix, _camCacheW, _camCacheH);
            return;
        }

        if (_webcam != null && _webcam.isPlaying)
        {
            if (_webcam.didUpdateThisFrame && _camPix != null &&
                _webcam.width > 8 && _webcam.height > 8 &&
                _camPix.Length == _webcam.width * _webcam.height)
            {
                _webcam.GetPixels32(_camPix);
                _lastCamPix = _camPix;
                _lastCamW = _webcam.width;
                _lastCamH = _webcam.height;
                _videoOk++;
            }

            if (_lastCamPix != null)
                _ui.Blit(dst, _lastCamPix, _lastCamW, _lastCamH);
            return;
        }

        if (_videoTex == null || _videoTex.width < 8) return;
        _ui.Blit(dst, _videoTex.GetPixels32(), _videoTex.width, _videoTex.height);
    }

    // ─────────────────────────── 姿态 ───────────────────────────

    bool TryGetHeadPose(out Quaternion rot, out Vector3 pos)
    {
        rot = Quaternion.identity;
        pos = Vector3.zero;

#if ENABLE_INPUT_SYSTEM
        try
        {
            var hmd = UnityEngine.InputSystem.InputSystem
                .GetDevice<UnityEngine.InputSystem.XR.XRHMD>();
            if (hmd != null && hmd.centerEyeRotation != null && hmd.centerEyePosition != null)
            {
                var r = hmd.centerEyeRotation.ReadValue();
                if (r != Quaternion.identity)
                {
                    rot = r;
                    pos = hmd.centerEyePosition.ReadValue();
                    _poseSrc = "xrhmd";
                    return true;
                }
            }
        }
        catch { }
#endif

        var cam = Camera.main;
        if (cam != null)
        {
            rot = cam.transform.rotation;
            pos = cam.transform.position;
            _poseSrc = "camera";
            return true;
        }
        _poseSrc = "none";
        return false;
    }

    bool TryGetHeadRay(out Ray ray)
    {
        ray = new Ray(Vector3.zero, Vector3.forward);
        if (!TryGetHeadPose(out Quaternion r, out Vector3 p)) return false;
        ray = new Ray(p, r * Vector3.forward);
        return true;
    }

    /// <summary>读手柄的 primaryButton（A / X）</summary>
    bool PrimaryButtonDown(UnityEngine.InputSystem.InputDevice dev)
    {
        try
        {
            foreach (var c in dev.allControls)
            {
                if (c.name.ToLowerInvariant() != "primarybutton") continue;
                var bc = c as UnityEngine.InputSystem.Controls.ButtonControl;
                return bc != null && bc.isPressed;
            }
        }
        catch { }
        return false;
    }

    bool TryGetHand(bool left, out Vector3 pos, out Quaternion rot, out bool pressed)
    {
        pos = Vector3.zero;
        rot = Quaternion.identity;
        pressed = false;

        string usage = left ? "LeftHand" : "RightHand";
#if ENABLE_INPUT_SYSTEM
        try
        {
            UnityEngine.InputSystem.InputDevice dev = null;
            foreach (var d in UnityEngine.InputSystem.InputSystem.devices)
            {
                if (!(d is UnityEngine.InputSystem.XR.XRController)) continue;
                foreach (var u in d.usages) if (u == usage) { dev = d; break; }
                if (dev != null) break;
            }
            if (dev == null) return false;

            var c = (UnityEngine.InputSystem.XR.XRController)dev;
            if (c.devicePosition == null || c.deviceRotation == null) return false;

            /* 位置一律用 devicePosition（手柄实体的位置）；
               方向优先 pointerRotation（瞄准方向），没有就退回 deviceRotation */
            pos = c.devicePosition.ReadValue();
            rot = c.deviceRotation.ReadValue();

            /* deviceRotation 是"握把"姿态；瞄准要用 pointer 姿态。
               这里单独 try/catch：pointer 读失败不影响手柄本身可用。 */
            try
            {
                foreach (var pc in dev.allControls)
                {
                    string pn = pc.name.ToLowerInvariant();
                    if (pn == "pointerposition")
                    {
                        var vp = pc as UnityEngine.InputSystem.Controls.Vector3Control;
                        if (vp != null) { _pointerPos = vp.ReadValue(); _usedPointer = true; }
                    }
                    else if (pn == "pointerrotation")
                    {
                        var qp = pc as UnityEngine.InputSystem.Controls.QuaternionControl;
                        if (qp != null) { rot = qp.ReadValue(); _usedPointer = true; }
                    }
                }
            }
            catch (Exception e) { Debug.LogWarning("[App] pointer 姿态读取失败: " + e.Message); }

            foreach (var ctl in dev.allControls)
            {
                string n = ctl.name;
                string ln = n.ToLowerInvariant();
                if (ln != "trigger" && ln != "triggerpressed" && ln != "grip"
                    && ln != "grippressed" && ln != "primarybutton" && ln != "secondarybutton") continue;

                bool v = false;
                if (ctl is UnityEngine.InputSystem.Controls.ButtonControl bc) v = bc.isPressed;
                else if (ctl is UnityEngine.InputSystem.Controls.AxisControl ac) v = ac.ReadValue() > 0.55f;

                if (ln == "primarybutton") _primaryDown = v;

                if (v && ln != _lastPressedControl)
                {
                    _lastPressedControl = ln;
                    Debug.Log("[App] 按下控件: " + n);
                }
                pressed |= v;
            }
            return true;
        }
        catch { }
#endif
        return false;
    }

    // ─────────────────────────── 配置文件（PC 可改，免重打） ───────────────────────────

    string CfgPath
    {
        get { return System.IO.Path.Combine(Application.persistentDataPath, "ht.cfg"); }
    }

    /// <summary>
    /// 读 ht.cfg（每行 key=value），PC 侧用 adb push 就能改，
    /// 不用为了一次参数调整重新打包。
    ///   down=0.45   dist=1.15   width=1.30   tilt=10   url=http://...   video=1
    /// </summary>
    void LoadCfgFile()
    {
        try
        {
            if (!System.IO.File.Exists(CfgPath))
            {
                Debug.Log("[App] 没有配置文件，用默认值；可写入 " + CfgPath);
                return;
            }

            foreach (var raw in System.IO.File.ReadAllLines(CfgPath))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("#")) continue;

                int eq = line.IndexOf('=');
                if (eq <= 0) continue;

                string k = line.Substring(0, eq).Trim().ToLowerInvariant();
                string v = line.Substring(eq + 1).Trim();

                float f;
                switch (k)
                {
                    case "down":  if (float.TryParse(v, out f)) downOffset = f; break;
                    case "dist":  if (float.TryParse(v, out f)) distM = f; break;
                    case "width": if (float.TryParse(v, out f)) widthM = f; break;
                    case "tilt":  if (float.TryParse(v, out f)) tiltDeg = f; break;
                    case "hz":    if (float.TryParse(v, out f)) sendHz = Mathf.Clamp((int)f, 30, 120); break;
                    case "follow": followHead = v == "1"; break;
                    case "pass": passthrough = v == "1"; break;
                    case "followk": if (float.TryParse(v, out f)) followK = Mathf.Clamp01(f); break;
                    case "flipx": _flipX = v == "1"; break;
                    case "flipy": _flipY = v == "1"; break;
                    case "src":   _useWebcam = v.ToLowerInvariant() == "webcam"; break;
                    case "cam":   _camPick = v; break;
                    case "camidx": if (int.TryParse(v, out int ci)) _camIdx = ci; break;
                    case "yawinv": _yawInv = v != "0"; break;
                    case "url":   videoUrl = v; break;
                    case "vsrc":  if (int.TryParse(v, out int vs)) _vsrc = vs; break;
                    case "pcip":  _pcIp = v; break;
                    case "video": videoOn = v != "0"; break;
                }
            }

            Debug.Log("[App] 已读配置 down=" + downOffset + " dist=" + distM +
                      " width=" + widthM + " tilt=" + tiltDeg +
                      " followK=" + followK + " flipX=" + _flipX + " flipY=" + _flipY);
        }
        catch (Exception e)
        {
            Debug.LogWarning("[App] 读配置失败: " + e.Message);
        }
    }

    // ─────────────────────────── 设置持久化 ───────────────────────────

    void LoadPrefs()
    {
        tiltDeg  = PlayerPrefs.GetFloat("ht.tilt", tiltDeg);
        distM    = PlayerPrefs.GetFloat("ht.dist", distM);
        widthM   = PlayerPrefs.GetFloat("ht.width", widthM);
        videoUrl = PlayerPrefs.GetString("ht.url", videoUrl);
        videoOn  = PlayerPrefs.GetInt("ht.video", videoOn ? 1 : 0) != 0;
    }

    void SavePrefs()
    {
        PlayerPrefs.SetFloat("ht.tilt", tiltDeg);
        PlayerPrefs.SetFloat("ht.dist", distM);
        PlayerPrefs.SetFloat("ht.width", widthM);
        PlayerPrefs.SetString("ht.url", videoUrl);
        PlayerPrefs.SetInt("ht.video", videoOn ? 1 : 0);
        PlayerPrefs.Save();
    }
}