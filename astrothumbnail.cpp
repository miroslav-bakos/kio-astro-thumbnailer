/*
 * FITS/XISF thumbnailer plugin for KDE Plasma 6 / Dolphin
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Runs entirely in-process using cfitsio and direct XISF binary parsing —
 * no subprocess, no QProcess fork(), bounded memory (~13 MB per request).
 */
#include <kio/thumbnailcreator.h>
#include <KPluginFactory>
#include <QFile>
#include <QImage>
#include <QXmlStreamReader>
#include <fitsio.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

static constexpr int MAX_DIM = 1024; // max pixels on longest side during processing

// ── Stretch ──────────────────────────────────────────────────────────────────

// Percentile of a float array (does not modify input, filters non-finite).
static float nthPercentile(const float *data, int n, float pct)
{
    std::vector<float> tmp;
    tmp.reserve(n);
    for (int i = 0; i < n; ++i)
        if (std::isfinite(data[i])) tmp.push_back(data[i]);
    if (tmp.empty()) return 0.f;
    int idx = std::clamp((int)(tmp.size() * (double)pct), 0, (int)tmp.size() - 1);
    std::nth_element(tmp.begin(), tmp.begin() + idx, tmp.end());
    return tmp[idx];
}

// CHW float buffer → QImage (Grayscale8 or RGB888) with percentile+arcsinh stretch.
// buf layout: buf[ch * sw * sh + row * sw + col]
static QImage planesCHWtoImage(const std::vector<float> &buf, int sw, int sh, int nch)
{
    const int n = sw * sh;
    std::vector<float> lo(nch), hi(nch);
    for (int ch = 0; ch < nch; ++ch) {
        const float *p = buf.data() + ch * n;
        lo[ch] = nthPercentile(p, n, 0.005f);
        hi[ch] = nthPercentile(p, n, 0.995f);
        if (hi[ch] <= lo[ch]) hi[ch] = lo[ch] + 1e-6f;
    }

    const float a = 0.6f;
    const float invScale = 1.0f / std::asinh(1.0f / a);

    auto fmt = (nch == 1) ? QImage::Format_Grayscale8 : QImage::Format_RGB888;
    QImage out(sw, sh, fmt);

    for (int row = 0; row < sh; ++row) {
        uchar *line = out.scanLine(row);
        for (int col = 0; col < sw; ++col) {
            for (int ch = 0; ch < nch; ++ch) {
                float v = buf[ch * n + row * sw + col];
                float norm = std::clamp((v - lo[ch]) / (hi[ch] - lo[ch]), 0.f, 1.f);
                float s = std::asinh(norm / a) * invScale;
                uchar u = (uchar)std::clamp(s * 255.f, 0.f, 255.f);
                if (nch == 1) line[col] = u;
                else          line[col * 3 + ch] = u;
            }
        }
    }
    return out;
}

// ── FITS ─────────────────────────────────────────────────────────────────────

static QImage thumbFromFits(const QString &path)
{
    fitsfile *ff = nullptr;
    int status = 0;

    fits_open_file(&ff, path.toLocal8Bit().constData(), READONLY, &status);
    if (status) return {};

    // Walk HDUs to find the first image with ≥ 2 axes
    int nHDUs = 0, naxis = 0;
    fits_get_num_hdus(ff, &nHDUs, &status);
    for (int h = 1; h <= nHDUs && status == 0; ++h) {
        int htype = 0;
        fits_movabs_hdu(ff, h, &htype, &status);
        if (htype != IMAGE_HDU) continue;
        fits_get_img_dim(ff, &naxis, &status);
        if (naxis >= 2) break;
    }
    if (status || naxis < 2 || naxis > 3) {
        fits_close_file(ff, &status);
        return {};
    }

    long naxisn[3] = {1, 1, 1};
    fits_get_img_size(ff, naxis, naxisn, &status);
    if (status) { fits_close_file(ff, &status); return {}; }

    long W = naxisn[0], H = naxisn[1];
    int  NCH = (naxis == 3) ? (int)std::min(naxisn[2], 3L) : 1;

    int  step = (int)std::max(1L, std::max(W, H) / (long)MAX_DIM);
    long sw = (W + step - 1) / step;
    long sh = (H + step - 1) / step;

    // fits_read_subset reads with step; output is Fortran-order (axis1 fastest)
    // which for [W, H, NCH] gives CHW layout in the flat array.
    long fpixel[3] = {1,    1,   1};
    long lpixel[3] = {W,    H,   NCH};
    long inc[3]    = {step, step, 1};

    std::vector<float> buf((size_t)(sw * sh * NCH), 0.f);
    fits_read_subset(ff, TFLOAT, fpixel, lpixel, inc,
                     nullptr, buf.data(), nullptr, &status);
    fits_close_file(ff, &status);

    if (status) return {};
    return planesCHWtoImage(buf, (int)sw, (int)sh, NCH);
}

// ── XISF ─────────────────────────────────────────────────────────────────────

struct XisfInfo {
    long    W = 0, H = 0, NCH = 0;
    qint64  offset = 0;
    int     bps    = 0;   // bytes per sample
    bool    isFloat = false;
    bool    planar  = true; // true = CHW (PixInsight default), false = HWC
};

static bool parseXisfHeader(QFile &f, XisfInfo &info)
{
    char sig[8];
    if (f.read(sig, 8) != 8 || std::memcmp(sig, "XISF0100", 8) != 0) return false;

    quint32 xmlLen = 0;
    if (f.read(reinterpret_cast<char *>(&xmlLen), 4) != 4) return false;
    f.seek(f.pos() + 4); // reserved
    xmlLen = qFromLittleEndian(xmlLen);

    QByteArray xml = f.read(xmlLen);
    if ((quint32)xml.size() != xmlLen) return false;

    QXmlStreamReader xr(xml);
    while (!xr.atEnd()) {
        xr.readNext();
        if (!xr.isStartElement() || xr.name() != QStringLiteral("Image")) continue;

        auto attrs = xr.attributes();
        QString loc = attrs.value("location").toString();
        if (!loc.startsWith(QLatin1String("attachment:"))) continue;

        QStringList lp = loc.split(':');
        if (lp.size() < 3) continue;

        QStringList gp = attrs.value("geometry").toString().split(':');
        if (gp.size() < 3) continue;

        info.W      = gp[0].toLong();
        info.H      = gp[1].toLong();
        info.NCH    = gp[2].toLong();
        info.offset = lp[1].toLongLong();

        const QString fmt = attrs.value("sampleFormat").toString();
        if      (fmt == "Float32") { info.bps = 4; info.isFloat = true; }
        else if (fmt == "Float64") { info.bps = 8; info.isFloat = true; }
        else if (fmt == "UInt16")  { info.bps = 2; info.isFloat = false; }
        else if (fmt == "UInt8")   { info.bps = 1; info.isFloat = false; }
        else continue;

        // PixInsight writes planar (CHW) storage even without an explicit attribute.
        // Only override if "Normal" (sample-interleaved / HWC) is explicitly set.
        info.planar = (attrs.value("pixelStorage").toString() != QStringLiteral("Normal"));

        return info.W > 0 && info.H > 0 && info.NCH > 0;
    }
    return false;
}

static inline float readSample(const char *p, int bps, bool isFloat)
{
    if (bps == 4 && isFloat) {
        float v; std::memcpy(&v, p, 4); return v;
    }
    if (bps == 8 && isFloat) {
        double v; std::memcpy(&v, p, 8); return (float)v;
    }
    if (bps == 2) {
        quint16 v; std::memcpy(&v, p, 2); return qFromLittleEndian(v) / 65535.f;
    }
    // UInt8
    return (uchar)*p / 255.f;
}

static QImage thumbFromXisf(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};

    XisfInfo info;
    if (!parseXisfHeader(f, info)) return {};

    int NCH  = (int)std::min(info.NCH, 3L);
    int step = (int)std::max(1L, std::max(info.W, info.H) / (long)MAX_DIM);
    int sw   = (int)((info.W + step - 1) / step);
    int sh   = (int)((info.H + step - 1) / step);

    std::vector<float> buf((size_t)(NCH * sw * sh), 0.f);

    if (info.planar) {
        // CHW: each channel occupies H*W contiguous samples
        qint64 planeBytes = (qint64)info.W * info.H * info.bps;
        std::vector<char> row((size_t)((qint64)info.W * info.bps));

        for (int ch = 0; ch < NCH; ++ch) {
            qint64 planeOff = info.offset + (qint64)ch * planeBytes;
            int dr = 0;
            for (long r = 0; r < info.H; r += step, ++dr) {
                if (!f.seek(planeOff + r * (qint64)info.W * info.bps)) break;
                if (f.read(row.data(), (qint64)row.size()) != (qint64)row.size()) break;
                float *dst = buf.data() + ch * sh * sw + dr * sw;
                int dc = 0;
                for (long c = 0; c < info.W; c += step, ++dc)
                    dst[dc] = readSample(row.data() + c * info.bps, info.bps, info.isFloat);
            }
        }
    } else {
        // HWC (sample-interleaved): each row has W*info.NCH samples
        qint64 rowBytes = (qint64)info.W * info.NCH * info.bps;
        std::vector<char> row((size_t)rowBytes);

        int dr = 0;
        for (long r = 0; r < info.H; r += step, ++dr) {
            if (!f.seek(info.offset + r * rowBytes)) break;
            if (f.read(row.data(), rowBytes) != rowBytes) break;
            int dc = 0;
            for (long c = 0; c < info.W; c += step, ++dc) {
                for (int ch = 0; ch < NCH; ++ch) {
                    const char *p = row.data() + (c * info.NCH + ch) * info.bps;
                    buf[ch * sh * sw + dr * sw + dc] =
                        readSample(p, info.bps, info.isFloat);
                }
            }
        }
    }

    return planesCHWtoImage(buf, sw, sh, NCH);
}

// ── Plugin ───────────────────────────────────────────────────────────────────

class AstroThumbnailCreator : public KIO::ThumbnailCreator
{
    Q_OBJECT
public:
    AstroThumbnailCreator(QObject *parent, const QVariantList &args)
        : KIO::ThumbnailCreator(parent, args) {}

    KIO::ThumbnailResult create(const KIO::ThumbnailRequest &request) override
    {
        const QString path = request.url().toLocalFile();
        if (path.isEmpty()) return KIO::ThumbnailResult::fail();

        const QString ext = path.section(QLatin1Char('.'), -1).toLower();
        const int targetSize =
            qMax(request.targetSize().width(), request.targetSize().height());

        QImage img;
        if      (ext == "fit" || ext == "fits" || ext == "fts") img = thumbFromFits(path);
        else if (ext == "xisf")                                  img = thumbFromXisf(path);

        if (img.isNull()) return KIO::ThumbnailResult::fail();

        if (img.width() > targetSize || img.height() > targetSize)
            img = img.scaled(targetSize, targetSize,
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);

        return KIO::ThumbnailResult::pass(img);
    }
};

K_PLUGIN_CLASS_WITH_JSON(AstroThumbnailCreator, "astrothumbnail.json")
#include "astrothumbnail.moc"
