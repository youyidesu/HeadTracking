using System.Collections.Generic;
using UnityEngine;

/// <summary>
/// 贴图 UI：所有内容（面板、按钮、文字、图传区域）都画在一张 Texture2D 上。
///
/// 这样做的理由：
///   · 不依赖 UGUI / Canvas / RectTransform / Font / Collider / EventSystem
///   · 唯一依赖是 "Unlit/Texture 能渲染" —— 这一点已经验证过（之前的白画布就是它）
///   · 命中判定用"射线与面板平面求交 → 换算成像素坐标"，纯数学，不碰物理系统
/// </summary>
public class TextureUi
{
    public int W { get; }
    public int H { get; }

    public Texture2D Tex { get; }

    /// <summary>贴图方向修正：贴到 Quad 上如果上下/左右翻了，用这两个开关扳回来</summary>
    public bool FlipX;
    public bool FlipY;
    Color32[] _buf;

    public struct Hit
    {
        public string id;
        public Vector2 pixel;
    }

    struct Btn
    {
        public string id;
        public RectInt rect;      /* 像素坐标，左下原点 */
    }

    readonly List<Btn> _btns = new List<Btn>();

    public TextureUi(int w, int h)
    {
        W = w;
        H = h;
        Tex = new Texture2D(w, h, TextureFormat.RGBA32, false);
        Tex.wrapMode = TextureWrapMode.Clamp;
        Tex.filterMode = FilterMode.Point;
        _buf = new Color32[w * h];
        Clear(new Color32(10, 16, 26, 255));
    }

    // ─────────────── 基础绘制（坐标系：左上原点，y 向下） ───────────────

    public void Clear(Color32 c)
    {
        for (int i = 0; i < _buf.Length; i++) _buf[i] = c;
    }

    public void Fill(RectInt r, Color32 c)
    {
        for (int y = r.yMin; y < r.yMax; y++)
        {
            if (y < 0 || y >= H) continue;
            for (int x = r.xMin; x < r.xMax; x++)
            {
                if (x < 0 || x >= W) continue;
                _buf[y * W + x] = c;
            }
        }
    }

    public void Border(RectInt r, Color32 c, int t = 2)
    {
        Fill(new RectInt(r.xMin, r.yMin, r.width, t), c);
        Fill(new RectInt(r.xMin, r.yMax - t, r.width, t), c);
        Fill(new RectInt(r.xMin, r.yMin, t, r.height), c);
        Fill(new RectInt(r.xMax - t, r.yMin, t, r.height), c);
    }

    /// <summary>画字（y 是文字顶部像素）</summary>
    public void Text(string s, int x, int y, int scale, Color32 c)
    {
        /* 贴图的 v 方向与屏幕相反，这里翻一下，调用者始终按"左上为原点"思考 */
        BitmapFont.Draw(s, x, H - y - BitmapFont.CharHeight(scale), scale, c, _buf, W, H);
    }

    public void TextCenter(string s, RectInt r, int scale, Color32 c)
    {
        int tw = BitmapFont.Measure(s, scale);
        int th = BitmapFont.CharHeight(scale);
        Text(s, r.xMin + (r.width - tw) / 2, r.yMin + (r.height - th) / 2, scale, c);
    }

    // ─────────────── 按钮 ───────────────

    public void Button(string id, RectInt r, string label, int scale,
                       Color32 face, Color32 edge, Color32 textCol, bool highlight)
    {
        Fill(r, highlight ? new Color32(60, 140, 90, 255) : face);
        Border(r, edge, 2);
        if (!string.IsNullOrEmpty(label)) TextCenter(label, r, scale, textCol);

        /* 同一个 id 覆盖旧的，避免重建界面后越积越多 */
        for (int i = 0; i < _btns.Count; i++)
        {
            if (_btns[i].id != id) continue;
            _btns[i] = new Btn { id = id, rect = r };
            return;
        }
        _btns.Add(new Btn { id = id, rect = r });
    }

    public void ClearButtons() { _btns.Clear(); }

    /// <summary>把一张源图按最近邻缩放拷进区域（用于图传画面）</summary>
    public void Blit(RectInt dst, Color32[] src, int sw, int sh)
    {
        if (src == null || sw < 2 || sh < 2) return;

        for (int y = 0; y < dst.height; y++)
        {
            int py = dst.yMin + y;
            if (py < 0 || py >= H) continue;

            /* UI 是 y 向下，贴图是 y 向上，这里翻一次 */
            int sy = sh - 1 - (y * sh / dst.height);
            if (sy < 0) sy = 0;
            if (sy >= sh) sy = sh - 1;

            int rowSrc = sy * sw;
            int rowDst = py * W;

            for (int x = 0; x < dst.width; x++)
            {
                int px = dst.xMin + x;
                if (px < 0 || px >= W) continue;
                _buf[rowDst + px] = src[rowSrc + (x * sw / dst.width)];
            }
        }
    }

    /// <summary>像素坐标 → 命中的按钮 id（没有则 null）</summary>
    public string Pick(float px, float py)
    {
        for (int i = 0; i < _btns.Count; i++)
        {
            var r = _btns[i].rect;
            if (px >= r.xMin && px < r.xMax && py >= r.yMin && py < r.yMax)
                return _btns[i].id;
        }
        return null;
    }

    /// <summary>
    /// 把当前画面水平镜像后写入另一张贴图。
    /// 背面那块 Quad 是转 180° 的，直接贴同一张图会"字反过来"，
    /// 镜像一次正好抵消。
    /// </summary>
    public void MirrorTo(Texture2D dst)
    {
        if (dst == null) return;

        var tmp = new Color32[_buf.Length];
        for (int y = 0; y < H; y++)
        {
            int row = y * W;
            for (int x = 0; x < W; x++) tmp[row + x] = _buf[row + (W - 1 - x)];
        }
        dst.SetPixels32(tmp);
        dst.Apply(false);
    }

    public void Apply()
    {
        Tex.SetPixels32(Transform(_buf, FlipX, FlipY));
        Tex.Apply(false);
    }

    Color32[] Transform(Color32[] src, bool fx, bool fy)
    {
        if (!fx && !fy) return src;

        var dst = new Color32[src.Length];
        for (int y = 0; y < H; y++)
        {
            int sy = fy ? (H - 1 - y) : y;
            for (int x = 0; x < W; x++)
            {
                int sx = fx ? (W - 1 - x) : x;
                dst[y * W + x] = src[sy * W + sx];
            }
        }
        return dst;
    }
}
