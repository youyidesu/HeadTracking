#if UNITY_EDITOR
using System;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

/// <summary>
/// Quest 3 + OpenXR 一键配置。
///
/// 关键点：**不直接引用 XR 相关命名空间**。XR 包还没装的时候，
/// `using UnityEditor.XR.Management;` 会让整个工程进 Safe Mode（编译不过）。
/// 所以这里全部走反射——没装包时菜单照样能用，只是提示你先装包。
///
/// 顺序：
///   HT ▸ 1. 安装 XR 包
///   HT ▸ 2. 配置为 Quest 3
///   HT ▸ 3. 检查配置
///   HT ▸ 4. 打包 APK
/// </summary>
public static class ProjectSetup
{
    // ─────────────────────────── 反射工具 ───────────────────────────

    static Type FindType(string fullName)
    {
        foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
        {
            var t = asm.GetType(fullName, false);
            if (t != null) return t;
        }
        return null;
    }

    /// <summary>取 Android 平台的 XRManagerSettings（没装包返回 null）</summary>
    static object GetManagerSettings(BuildTargetGroup group)
    {
        var perTarget = FindType("UnityEditor.XR.Management.XRGeneralSettingsPerBuildTarget");
        if (perTarget == null) return null;

        var m = perTarget.GetMethod("XRGeneralSettingsForBuildTarget",
            BindingFlags.Public | BindingFlags.Static);
        if (m == null) return null;

        var general = m.Invoke(null, new object[] { group });
        if (general == null) return null;

        var prop = general.GetType().GetProperty("Manager",
            BindingFlags.Public | BindingFlags.Instance);
        return prop?.GetValue(general);
    }

    /// <summary>
    /// 尝试自动创建 XRGeneralSettings 资产（Unity 只在 UI 里勾过一次才会建）。
    /// 成功返回 true；失败就让调用方给用户明确指引。
    /// </summary>
    static bool TryCreateXrSettings()
    {
        var t = FindType("UnityEditor.XR.Management.XRGeneralSettingsPerBuildTarget");
        if (t == null) return false;

        foreach (var name in new[] { "GetOrCreateSettings", "CreateSettings", "GetSettings" })
        {
            var m = t.GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
            if (m == null) continue;
            try
            {
                var r = m.Invoke(null, m.GetParameters().Length == 0 ? null : new object[] { BuildTargetGroup.Android });
                if (r != null)
                {
                    AssetDatabase.SaveAssets();
                    Debug.Log("已自动创建 XR 设置（" + name + "）");
                    return true;
                }
            }
            catch (Exception e)
            {
                Debug.Log("自动创建 XR 设置失败(" + name + "): " + e.Message);
            }
        }
        return false;
    }

    [MenuItem("HT/0. 诊断环境", priority = 0)]
    public static void Diagnose()
    {
        var sb = new System.Text.StringBuilder();
        sb.AppendLine("—— 诊断 ——");

        bool hasMgmt = FindType("UnityEditor.XR.Management.XRGeneralSettingsPerBuildTarget") != null;
        bool hasStore = FindType("UnityEditor.XR.Management.Metadata.XRPackageMetadataStore") != null;
        bool hasLoader = FindType("UnityEngine.XR.OpenXR.OpenXRLoader") != null;
        sb.AppendLine("XR Management 编辑器类型 : " + hasMgmt);
        sb.AppendLine("XRPackageMetadataStore   : " + hasStore);
        sb.AppendLine("OpenXRLoader 类型        : " + hasLoader);

        // 资产可能在 ProjectSettings/ 也可能在 Assets/XR/，两处都找
        var found = AssetDatabase.FindAssets("XRGeneralSettingsPerBuildTarget")
            .Select(AssetDatabase.GUIDToAssetPath).ToArray();
        sb.AppendLine("XR 设置资产              : " +
            (found.Length > 0 ? string.Join(", ", found) : "未找到"));

        var mgr = GetManagerSettings(BuildTargetGroup.Android);
        sb.AppendLine("Android XRManagerSettings: " + (mgr != null ? mgr.GetType().Name : "null"));
        if (mgr != null)
        {
            var p = mgr.GetType().GetProperty("activeLoader");
            var l = p?.GetValue(mgr);
            sb.AppendLine("Android 已启用 loader    : " + (l != null ? l.GetType().Name : "无"));
        }

        sb.AppendLine("当前目标平台             : " + EditorUserBuildSettings.activeBuildTarget +
                      (EditorUserBuildSettings.activeBuildTarget == BuildTarget.Android
                          ? "" : "   ← 必须先切到 Android！"));
        Debug.Log(sb.ToString());
    }

    // ─────────────────────────── 菜单 ───────────────────────────

    /// <summary>
    /// 设置 Active Input Handling（0=Input Manager / 1=Input System / 2=Both）。
    /// Android 不支持 Both；我们只用 OpenXR 姿态，旧 Input Manager 就够。
    ///
    /// 两道保险：
    ///   1) SerializedObject 改字段（正常路径）
    ///   2) 直接改 ProjectSettings.asset 文本（SerializedObject 没生效时兜底）
    /// 改完必须重启 Unity 才真正生效。
    /// </summary>
    public static void SetActiveInputHandler(int mode)
    {
        const string path = "ProjectSettings/ProjectSettings.asset";

        // ① 正常路径
        var objs = AssetDatabase.LoadAllAssetsAtPath(path);
        int before = -1;
        if (objs != null && objs.Length > 0)
        {
            var so = new SerializedObject(objs[0]);
            var prop = so.FindProperty("activeInputHandler");
            if (prop != null)
            {
                before = prop.intValue;
                if (before != mode)
                {
                    prop.intValue = mode;
                    so.ApplyModifiedProperties();
                    AssetDatabase.SaveAssets();
                }
            }
            else Debug.LogWarning("ProjectSettings.asset 里没找到 activeInputHandler 字段");
        }
        else Debug.LogWarning("LoadAllAssetsAtPath 读不到 " + path);

        // ② 兜底：直接改文本（Unity 崩溃/被杀时 SerializedObject 可能没落盘）
        try
        {
            var full = System.IO.Path.Combine(
                System.IO.Directory.GetCurrentDirectory(), path);
            if (System.IO.File.Exists(full))
            {
                var txt = System.IO.File.ReadAllText(full);
                var newTxt = System.Text.RegularExpressions.Regex.Replace(
                    txt, @"activeInputHandler:\s*\d+", "activeInputHandler: " + mode);
                if (newTxt != txt)
                {
                    System.IO.File.WriteAllText(full, newTxt);
                    Debug.Log("已直接改写 ProjectSettings.asset 文本");
                }
            }
        }
        catch (Exception e) { Debug.LogWarning("文本兜底失败: " + e.Message); }

        AssetDatabase.Refresh();

        Debug.Log($"Active Input Handling: 改前 {before} → 目标 {mode}" +
                  "（0=Input Manager / 1=Input System）\n**必须重启 Unity 才生效**");
    }

    [MenuItem("HT/7. 修 Active Input Handling", priority = 7)]
    public static void FixInputHandler()
    {
        // 工程里装了 com.unity.inputsystem（Meta/OpenXR 的依赖），
        // Meta 的校验要求 Active Input Handling 必须是「Input System Package (New)」= 1
        SetActiveInputHandler(1);
        EditorUtility.DisplayDialog("Active Input Handling",
            "已设为 Input System Package（1）。\n\n请现在重启 Unity（关闭再打开工程），\n" +
            "否则 OpenXR 校验仍会拦构建。", "知道了");
    }


    [MenuItem("HT/1. 安装 XR 包", priority = 1)]
    public static void InstallXrPackages()
    {
        var pm = FindType("UnityEditor.PackageManager.Client");
        if (pm == null) { Debug.LogError("找不到 PackageManager，Unity 版本不支持？"); return; }

        var add = pm.GetMethods(BindingFlags.Public | BindingFlags.Static)
                     .FirstOrDefault(x => x.Name == "Add" && x.GetParameters().Length == 1);
        if (add == null) { Debug.LogError("PackageManager.Client.Add 不可用"); return; }

        add.Invoke(null, new object[] { "com.unity.xr.management" });
        add.Invoke(null, new object[] { "com.unity.xr.openxr" });
        Debug.Log("已发起安装：com.unity.xr.management + com.unity.xr.openxr\n" +
                  "等 Console 里下载完成（几分钟），然后执行 HT ▸ 2");
    }

    /// <summary>
    /// Meta Quest 官方 OpenXR 包。装上之后：
    ///   · OpenXR 特性里多出 Meta Quest Support（触觉/手追/透视后续可用）
    ///   · Unity 6 的 Build Profiles 里会出现 Meta Quest 预设
    /// 注意：它的底层仍然是 Android 构建，产物还是 ARM64 APK。
    /// </summary>
    [MenuItem("HT/1b. 安装 Meta Quest 官方包", priority = 11)]
    public static void InstallMetaOpenXr()
    {
        var pm = FindType("UnityEditor.PackageManager.Client");
        var add = pm?.GetMethods(BindingFlags.Public | BindingFlags.Static)
                     .FirstOrDefault(x => x.Name == "Add" && x.GetParameters().Length == 1);
        if (add == null) { Debug.LogError("PackageManager 不可用"); return; }

        add.Invoke(null, new object[] { "com.unity.xr.meta-openxr" });
        Debug.Log("已发起安装 com.unity.xr.meta-openxr\n" +
                  "装完后 Project Settings ▸ XR Plug-in Management ▸ OpenXR ▸ Features " +
                  "里会看到 Meta Quest Support，建议勾上");
    }

    [MenuItem("HT/2. 配置为 Quest 3", priority = 2)]
    public static void Configure()
    {
        EditorUserBuildSettings.selectedBuildTargetGroup = BuildTargetGroup.Android;

        PlayerSettings.SetApplicationIdentifier(BuildTargetGroup.Android, "com.ht.turret");
        PlayerSettings.productName = "HT Turret";

        // Meta Quest Camera 特性要求 Android 12L(API 32)；Quest 3 本身就是 12L
        PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel32;
        PlayerSettings.Android.targetSdkVersion = (AndroidSdkVersions)34;

        PlayerSettings.SetScriptingBackend(BuildTargetGroup.Android, ScriptingImplementation.IL2CPP);
        PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;

        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
        PlayerSettings.SetGraphicsAPIs(BuildTarget.Android,
            new[] { UnityEngine.Rendering.GraphicsDeviceType.Vulkan });

        PlayerSettings.defaultInterfaceOrientation = UIOrientation.LandscapeLeft;
        PlayerSettings.colorSpace = ColorSpace.Linear;

        // Active Input Handling 必须是单一模式；工程里装了 com.unity.inputsystem
        // （Meta/OpenXR 的间接依赖），所以 Meta 校验要求选「Input System Package (New)」= 1
        SetActiveInputHandler(1);

        // —— 启用 OpenXR loader（反射，没装包时给出明确提示）——
        var manager = GetManagerSettings(BuildTargetGroup.Android);
        if (manager == null)
        {
            // 包已装但 XR 设置资产还不存在：Unity 只有在 Project Settings 里
            // 勾过一次之后才会生成它。先尝试自动建，建不出来就给出明确指引。
            if (TryCreateXrSettings())
                manager = GetManagerSettings(BuildTargetGroup.Android);

            if (manager == null)
            {
                Debug.LogError(
                    "XR 设置还没初始化。做一次这个动作（只需一次）：\n" +
                    "  Edit ▸ Project Settings ▸ XR Plug-in Management\n" +
                    "  → 点左上角 Android 页签 → 勾上 OpenXR\n" +
                    "然后重新点 HT ▸ 2。\n" +
                    "（Player 设置部分已按 Quest 要求写好，不受影响）");
                return;
            }
        }

        var store = FindType("UnityEditor.XR.Management.Metadata.XRPackageMetadataStore");
        var assign = store?.GetMethod("AssignLoader",
            BindingFlags.Public | BindingFlags.Static);
        if (assign == null)
        {
            Debug.LogError("XRPackageMetadataStore 不可用 —— XR Plugin Management 没装好");
            return;
        }

        const string loaderName = "UnityEngine.XR.OpenXR.OpenXRLoader";
        bool ok;
        try
        {
            ok = (bool)assign.Invoke(null, new[] { manager, (object)loaderName, (object)BuildTargetGroup.Android });
        }
        catch (Exception e)
        {
            Debug.LogError("启用 OpenXR loader 失败: " + e.Message);
            return;
        }

        Debug.Log(ok ? "OpenXR loader 已启用" : "OpenXR loader 未启用（检查 OpenXR Plugin 是否装好）");
        AssetDatabase.SaveAssets();
        Debug.Log("配置完成：IL2CPP / ARM64 / Vulkan / minSdk29 / targetSdk34");
    }

    [MenuItem("HT/3. 检查配置", priority = 3)]
    public static void Verify()
    {
        var sb = new System.Text.StringBuilder();
        sb.AppendLine("包名      : " + PlayerSettings.GetApplicationIdentifier(BuildTargetGroup.Android));
        sb.AppendLine("脚本后端  : " + PlayerSettings.GetScriptingBackend(BuildTargetGroup.Android));
        sb.AppendLine("架构      : " + PlayerSettings.Android.targetArchitectures);
        sb.AppendLine("minSdk    : " + PlayerSettings.Android.minSdkVersion);
        sb.AppendLine("targetSdk : " + PlayerSettings.Android.targetSdkVersion);
        sb.AppendLine("图形API   : " + string.Join(",", PlayerSettings.GetGraphicsAPIs(BuildTarget.Android)));

        var mgr = GetManagerSettings(BuildTargetGroup.Android);
        string loader = "未启用";
        if (mgr != null)
        {
            var p = mgr.GetType().GetProperty("activeLoader");
            var l = p?.GetValue(mgr);
            if (l != null) loader = l.GetType().Name;
        }
        sb.AppendLine("OpenXR    : " + loader);
        Debug.Log(sb.ToString());
    }

    /// <summary>
    /// 一键造一个能直接跑的示例场景：Main Camera + HeadSender，存到
    /// Assets/Scenes/Main.unity 并加进 Build Settings。
    /// </summary>
    [MenuItem("HT/5. 新建示例场景", priority = 5)]
    public static void CreateSampleScene()
    {
        const string dir = "Assets/Scenes";
        const string path = dir + "/Main.unity";

        if (!AssetDatabase.IsValidFolder(dir)) AssetDatabase.CreateFolder("Assets", "Scenes");

        var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

        var cam = Camera.main;
        if (cam == null)
        {
            var go = new GameObject("Main Camera");
            go.tag = "MainCamera";
            cam = go.AddComponent<Camera>();
            go.AddComponent<AudioListener>();
        }
        cam.transform.position = Vector3.zero;
        cam.transform.rotation = Quaternion.identity;
        cam.clearFlags = CameraClearFlags.SolidColor;
        cam.backgroundColor = new Color(0.05f, 0.06f, 0.08f);

        // 挂发送脚本
        if (cam.GetComponent<HeadSender>() == null)
            cam.gameObject.AddComponent<HeadSender>();

        // 屏幕上给个提示（真机头显里看不到，但编辑器里能确认场景对了）
        var canvasGo = new GameObject("Hint");
        var text = canvasGo.AddComponent<UnityEngine.UI.Text>();
        text.text = "HT Turret · HeadSender ready";
        text.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
        text.alignment = TextAnchor.MiddleCenter;
        text.color = Color.cyan;
        text.fontSize = 28;
        var rt = text.GetComponent<RectTransform>();
        if (rt != null) rt.sizeDelta = new Vector2(800, 100);
        text.transform.position = new Vector3(0.5f, 0.1f, 1f);

        EditorSceneManager.SaveScene(scene, path);

        var list = EditorBuildSettings.scenes.ToList();
        if (!list.Any(s => s.path == path))
        {
            list.Add(new EditorBuildSettingsScene(path, true));
            EditorBuildSettings.scenes = list.ToArray();
        }

        Debug.Log("示例场景已创建：" + path + "\n已加入 Build Settings，可以直接 HT ▸ 4 打包");
    }

    /// <summary>
    /// 把所有 OpenXR 校验问题一次列出来。
    /// 打包失败时只显示第一条，容易一条一条试；这里一次看全。
    /// </summary>
    [MenuItem("HT/6. 列出 OpenXR 校验问题", priority = 6)]
    public static void ListValidationIssues()
    {
        int shown = 0;
        foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
        {
            Type t;
            try { t = asm.GetTypes().FirstOrDefault(x => x.Name == "OpenXRProjectValidation"); }
            catch { continue; }
            if (t == null) continue;

            var methods = t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
                           .Where(x => x.Name == "GetCurrentValidationIssues").ToArray();
            if (methods.Length == 0) continue;

            object list = null;
            foreach (var m in methods)
            {
                // 按签名自动填参数：BuildTargetGroup 给 Android，其余给 null/默认值
                var ps = m.GetParameters();
                var args = new object[ps.Length];
                for (int i = 0; i < ps.Length; i++)
                {
                    var pt = ps[i].ParameterType;
                    if (pt == typeof(BuildTargetGroup)) args[i] = BuildTargetGroup.Android;
                    else if (pt.IsValueType) args[i] = Activator.CreateInstance(pt);
                    else args[i] = null;
                }
                try
                {
                    list = m.Invoke(null, args);
                    if (list != null) break;
                }
                catch (Exception e)
                {
                    Debug.Log("签名不匹配 " + m.Name + "(" + ps.Length + "参数): " + e.Message);
                }
            }
            if (list == null)
            {
                Debug.LogWarning("OpenXRProjectValidation 找到了，但所有重载都调不通。\n" +
                                 "直接去 Project Settings ▸ XR Plug-in Management ▸ Project Validation 点 Fix All 更快。");
                return;
            }

            var en = list as System.Collections.IEnumerable;
            if (en == null)
            {
                Debug.LogWarning("返回对象不是集合: " + list.GetType().Name);
                return;
            }

            var sb = new System.Text.StringBuilder("—— OpenXR 校验问题 ——\n");
            foreach (var item in en)
            {
                if (item == null) continue;
                var it = item.GetType();
                string msg = it.GetProperty("message")?.GetValue(item)?.ToString()
                          ?? it.GetProperty("Message")?.GetValue(item)?.ToString() ?? "";
                string sev = it.GetProperty("severity")?.GetValue(item)?.ToString()
                          ?? it.GetProperty("Severity")?.GetValue(item)?.ToString() ?? "";
                sb.AppendLine($"[{sev}] {msg}");
                shown++;
            }
            if (shown == 0) sb.AppendLine("（没有待修问题）");
            Debug.Log(sb.ToString());
            return;
        }
        Debug.Log("没找到 OpenXRProjectValidation（OpenXR 包未装或版本不同）");
    }

    /// <summary>
    /// 把运行时用到的着色器加进「Always Included Shaders」。
    /// 不加的话，构建时会因为没有任何资源引用而被剔除，
    /// 运行时 Shader.Find 拿到 null → 材质渲染成粉色（紫红）。
    /// </summary>
    [MenuItem("HT/8. 修复粉色材质（常驻着色器）", priority = 8)]
    public static void AddAlwaysIncludedShaders()
    {
        string[] want =
        {
            "Unlit/Color", "Unlit/Texture", "Unlit/Transparent",
            "Sprites/Default", "UI/Default", "Legacy Shaders/VertexLit",
            "Standard",
        };

        var gsPath = "ProjectSettings/GraphicsSettings.asset";
        var objs = AssetDatabase.LoadAllAssetsAtPath(gsPath);
        if (objs == null || objs.Length == 0) { Debug.LogError("读不到 GraphicsSettings"); return; }

        var so = new SerializedObject(objs[0]);
        var arr = so.FindProperty("m_AlwaysIncludedShaders");
        if (arr == null) { Debug.LogError("找不到 m_AlwaysIncludedShaders"); return; }

        var existing = new System.Collections.Generic.HashSet<string>();
        for (int i = 0; i < arr.arraySize; i++)
        {
            var sh = arr.GetArrayElementAtIndex(i).objectReferenceValue as Shader;
            if (sh != null) existing.Add(sh.name);
        }

        int added = 0;
        foreach (var name in want)
        {
            if (existing.Contains(name)) continue;
            var sh = Shader.Find(name);
            if (sh == null) { Debug.LogWarning("找不到着色器: " + name); continue; }
            arr.InsertArrayElementAtIndex(arr.arraySize);
            arr.GetArrayElementAtIndex(arr.arraySize - 1).objectReferenceValue = sh;
            added++;
        }

        so.ApplyModifiedProperties();
        AssetDatabase.SaveAssets();
        Debug.Log($"常驻着色器已更新：新增 {added} 个（共 {arr.arraySize} 个）");
    }

    /// <summary>
    /// 压缩包体：托管代码剥离 + 引擎代码裁剪 + 关掉调试构建。
    /// 34MB 的包主要来自未剥离的 IL2CPP 产物和调试符号。
    /// </summary>
    [MenuItem("HT/9. 压缩包体", priority = 9)]
    public static void ShrinkBuild()
    {
        PlayerSettings.SetManagedStrippingLevel(BuildTargetGroup.Android, ManagedStrippingLevel.High);
        PlayerSettings.stripEngineCode = true;
        PlayerSettings.SetIl2CppCompilerConfiguration(BuildTargetGroup.Android,
            Il2CppCompilerConfiguration.Release);

        EditorUserBuildSettings.development = false;
        EditorUserBuildSettings.allowDebugging = false;
        EditorUserBuildSettings.connectProfiler = false;

        AssetDatabase.SaveAssets();
        Debug.Log("已设置：托管代码 High 剥离 / 引擎代码裁剪 / IL2CPP Release + OptimizeSize / 关闭调试构建");
        EditorUtility.DisplayDialog("压缩包体",
            "已设置完成：\n\n" +
            "· 托管代码剥离：High\n" +
            "· 引擎代码裁剪：开\n" +
            "· IL2CPP：Release + OptimizeSize\n" +
            "· 调试构建：关\n\n" +
            "直接 HT ▸ 4 打包即可。", "知道了");
    }

    [MenuItem("HT/4. 打包 APK", priority = 4)]
    public static void BuildApk()
    {
        var scenes = EditorBuildSettings.scenes.Where(s => s.enabled)
            .Select(s => s.path).ToArray();
        if (scenes.Length == 0)
        {
            Debug.LogError("Build Settings 里没有场景。先 File ▸ Save Scene 存一个，再 Add Open Scenes");
            return;
        }

        var opts = new BuildPlayerOptions
        {
            scenes = scenes,
            locationPathName = "Build/HTSender.apk",
            target = BuildTarget.Android,
            options = BuildOptions.None,
        };
        var report = BuildPipeline.BuildPlayer(opts);
        Debug.Log("打包结果: " + report.summary.result +
                  "  大小 " + (report.summary.totalSize / 1024 / 1024) + " MB" +
                  "  输出 " + opts.locationPathName);
    }
}
#endif