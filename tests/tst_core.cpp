#include "FfmpegArgs.h"
#include "FfmpegInstaller.h"
#include "Policy.h"
#include "Probe.h"
#include "Settings.h"
#include "ShareLink.h"
#include "Tools.h"

#include <QFile>
#include <QTest>

#include <algorithm>

using namespace tt;

namespace {
QByteArray readData(const QString &name)
{
    QFile f(QStringLiteral(TT_TEST_DATA_DIR "/") + name);
    if (!f.open(QIODevice::ReadOnly))
        qFatal("cannot open test data %s", qPrintable(name));
    return f.readAll();
}

QString argAfter(const QStringList &args, const QString &key)
{
    const qsizetype i = args.indexOf(key);
    return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString();
}

ProbeResult goodOutput(const ProbeResult &src)
{
    ProbeResult out;
    out.hasVideo = true;
    out.vcodec = QStringLiteral("hevc");
    out.vprofile = QStringLiteral("Main 10");
    out.vtag = QStringLiteral("hvc1");
    out.duration = src.duration + 0.02;
    out.audioStreams = 1;
    return out;
}
} // namespace

class TestCore : public QObject
{
    Q_OBJECT
private slots:
    void shareLink_data()
    {
        QTest::addColumn<QString>("link");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("base");
        QTest::newRow("folder") << "https://my.hidrive.com/share/abc123XYZ0#$/TeamA/Clips/2026-01" << true
                                << "abc123XYZ0" << "/TeamA/Clips/2026-01";
        QTest::newRow("root") << "https://my.hidrive.com/share/abc123XYZ0" << true << "abc123XYZ0" << "/";
        QTest::newRow("encoded+slash") << "https://my.hidrive.com/share/abc123XYZ0#$/Music%20%28v2%29/" << true
                                       << "abc123XYZ0" << "/Music (v2)";
        QTest::newRow("whitespace") << "  https://my.hidrive.com/share/k9#$/A  \n" << true << "k9" << "/A";
        QTest::newRow("dotdot") << "https://my.hidrive.com/share/abc#$/a/../b" << false << "" << "";
        QTest::newRow("not a share") << "https://example.com/foo" << false << "" << "";
    }
    void shareLink()
    {
        QFETCH(QString, link);
        QFETCH(bool, valid);
        QFETCH(QString, id);
        QFETCH(QString, base);
        const auto parsed = parseShareLink(link);
        QCOMPARE(parsed.has_value(), valid);
        if (valid) {
            QCOMPARE(parsed->shareId, id);
            QCOMPARE(parsed->basePath, base);
        }
    }

    void insideBase()
    {
        QVERIFY(isInsideBase(QStringLiteral("/TeamA/Test"), QStringLiteral("/TeamA/Test/x.mp4")));
        QVERIFY(isInsideBase(QStringLiteral("/TeamA/Test"), QStringLiteral("/TeamA/Test")));
        QVERIFY(!isInsideBase(QStringLiteral("/TeamA/Test"), QStringLiteral("/TeamA/Testx/y.mp4")));
        QVERIFY(!isInsideBase(QStringLiteral("/TeamA/Test"), QStringLiteral("/TeamA/Test/../Clips/a.mp4")));
        QVERIFY(!isInsideBase(QStringLiteral("/TeamA/Test"), QStringLiteral("/TeamA/Clips/a.mp4")));
        QVERIFY(isInsideBase(QStringLiteral("/"), QStringLiteral("/anything/at/all")));
        QCOMPARE(relativeToBase(QStringLiteral("/a/b"), QStringLiteral("/a/b/c/d.mp4")), QStringLiteral("c/d.mp4"));
        QCOMPARE(relativeToBase(QStringLiteral("/"), QStringLiteral("/c/d.mp4")), QStringLiteral("c/d.mp4"));
        QCOMPARE(remoteParent(QStringLiteral("/a/b/c.mp4")), QStringLiteral("/a/b"));
        QCOMPARE(remoteName(QStringLiteral("/a/b/c.mp4")), QStringLiteral("c.mp4"));
    }

    void localMirror_data()
    {
        QTest::addColumn<QString>("relative");
        QTest::addColumn<QString>("expected");
        QTest::newRow("plain") << "a/b/c.mp4" << "C:/out/a/b/c.mp4";
        QTest::newRow("forbidden chars") << "x:y/what?*.mp4" << "C:/out/x_y/what__.mp4";
        QTest::newRow("backslash dotdot") << "..\\..\\evil/c.mp4" << "C:/out/.._.._evil/c.mp4";
        QTest::newRow("dotdot segment") << "../../c.mp4" << "C:/out/_/_/c.mp4";
        QTest::newRow("trailing dots") << "dir. ./c.mp4" << "C:/out/dir/c.mp4";
        QTest::newRow("device name") << "con/aux.mp4" << "C:/out/_con/_aux.mp4";
        QTest::newRow("empty") << "" << "";
    }
    void localMirror()
    {
        QFETCH(QString, relative);
        QFETCH(QString, expected);
        QCOMPARE(localMirrorPath(QStringLiteral("C:\\out"), relative), expected);
    }

    void ffmpegDownloadChoice()
    {
        const QByteArray sums = "663cded3782f4aaf1e5b44b8ff1112d378f57e824afcc5ea6fb35062e5dd8590  ffmpeg-master-latest-win64-gpl-shared.zip\r\n"
                                "E7532E780A277968093F20477E57074908A4F6D43F4A820E71E7CAAB28EEC690 *ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip\n"
                                "2b7bd492859505e52cc76304de0ce415eea881aa679557f6498f96b1b19973de  ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip\n"
                                "not a checksum line\n";
        const auto parsed = parseChecksums(sums);
        QCOMPARE(parsed.size(), 3);
        QCOMPARE(parsed.value(QStringLiteral("ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip")),
                 QStringLiteral("e7532e780a277968093f20477e57074908a4f6d43f4a820e71e7caab28eec690"));
        QCOMPARE(pickFfmpegAsset(parsed.keys()), QString::fromLatin1(kFfmpegPreferredAsset));

        // pinned branch gone: newest release branch, never master or a static/LGPL build
        const QStringList later{QStringLiteral("ffmpeg-master-latest-win64-gpl-shared.zip"),
                                QStringLiteral("ffmpeg-n9.1-latest-win64-gpl-shared-9.1.zip"),
                                QStringLiteral("ffmpeg-n10.0-latest-win64-gpl-shared-10.0.zip"),
                                QStringLiteral("ffmpeg-n11.0-latest-win64-lgpl-shared-11.0.zip"),
                                QStringLiteral("ffmpeg-n11.0-latest-win64-gpl-11.0.zip")};
        QCOMPARE(pickFfmpegAsset(later), QStringLiteral("ffmpeg-n10.0-latest-win64-gpl-shared-10.0.zip"));
        QCOMPARE(pickFfmpegAsset({QStringLiteral("ffmpeg-master-latest-win64-gpl-shared.zip")}), QString());
    }

    void toolSettings()
    {
        Settings s;
        QVERIFY(toolsAutomatic(s));
        s.ffmpegPath = QStringLiteral("ffmpeg"); // value stored by older versions
        s.ffprobePath = QStringLiteral("ffprobe.exe");
        QVERIFY(toolsAutomatic(s));
        s.ffmpegPath = QStringLiteral("D:/tools/ffmpeg.exe");
        QVERIFY(!toolsAutomatic(s));
        const ToolLocation missing = locateTools(s);
        QVERIFY(!missing.ok());
        QCOMPARE(missing.ffprobe, QStringLiteral("D:/tools/ffprobe.exe")); // next to ffmpeg
    }

    void probeSamsung()
    {
        const ProbeResult p = parseProbeJson(readData(QStringLiteral("samsung_4k60_hlg.json")));
        QVERIFY2(p.ok(), qPrintable(p.error));
        QVERIFY(p.hasVideo);
        QCOMPARE(p.videoIndex, 0);
        QCOMPARE(p.vcodec, QStringLiteral("hevc"));
        QCOMPARE(p.width, 3840);
        QCOMPARE(p.height, 2160);
        QVERIFY(p.fps > 59 && p.fps < 61);
        QCOMPARE(p.colorTransfer, QStringLiteral("arib-std-b67"));
        QCOMPARE(p.hdrLabel(), QStringLiteral("HLG"));
        QCOMPARE(p.videoBitrate, 51799510);
        QCOMPARE(p.audioIndex, 1);
        QVERIFY(p.audioCopy);
        QVERIFY(!p.taggedByUs());
    }

    void probeIphone()
    {
        const ProbeResult p = parseProbeJson(readData(QStringLiteral("iphone_1080p30_apac.json")));
        QVERIFY2(p.ok(), qPrintable(p.error));
        QCOMPARE(p.videoIndex, 2);
        QCOMPARE(p.vcodec, QStringLiteral("h264"));
        QCOMPARE(p.shortSide(), 1080);
        QVERIFY(p.fps > 29 && p.fps < 30.5);
        QCOMPARE(p.audioIndex, 0); // AAC, not the APAC track
        QVERIFY(p.audioCopy);
        QCOMPARE(p.audioStreams, 2);
        QCOMPARE(p.hdrLabel(), QString());
    }

    void decisions()
    {
        const Settings s;
        const ProbeResult samsung = parseProbeJson(readData(QStringLiteral("samsung_4k60_hlg.json")));
        Decision d = decide(samsung, s);
        QCOMPARE(d.kind, Decision::Kind::Transcode);
        QVERIFY(d.highFps);
        QCOMPARE(d.thresholdBps, 24'000'000);
        QCOMPARE(d.capBps, 18'000'000);

        const ProbeResult iphone = parseProbeJson(readData(QStringLiteral("iphone_1080p30_apac.json")));
        d = decide(iphone, s);
        QCOMPARE(d.kind, Decision::Kind::Transcode);
        QVERIFY(!d.highFps);
        QCOMPARE(d.thresholdBps, 6'000'000);
        QCOMPARE(d.capBps, 4'500'000);

        // small messenger clip: 848x478, 30 fps, 1.1 Mbit/s
        d = decide(478, 30.004, 1'105'020, true, false, s);
        QCOMPARE(d.kind, Decision::Kind::SmallEnough);
        QCOMPARE(d.thresholdBps, 3'000'000);

        // portrait 1440p, 60 fps, already ours
        d = decide(1440, 60, 50'000'000, true, true, s);
        QCOMPARE(d.kind, Decision::Kind::AlreadyOurs);
        QCOMPARE(d.ruleIndex, 1);

        d = decide(0, 0, 0, false, false, s);
        QCOMPARE(d.kind, Decision::Kind::NotVideo);
    }

    void ruleOrder()
    {
        QList<ResolutionRule> rules = defaultResolutionRules();
        std::ranges::reverse(rules); // the lookup must not depend on the order
        QCOMPARE(rules[ruleIndexForShortSide(rules, 2160)].label, QStringLiteral("2160p"));
        QCOMPARE(rules[ruleIndexForShortSide(rules, 1440)].label, QStringLiteral("1440p"));
        QCOMPARE(rules[ruleIndexForShortSide(rules, 1080)].label, QStringLiteral("1080p"));
        QCOMPARE(rules[ruleIndexForShortSide(rules, 720)].label, QStringLiteral("<1080p"));
        QCOMPARE(ruleIndexForShortSide({}, 720), -1);
    }

    void ffmpegArgsSamsung()
    {
        const Settings s;
        const ProbeResult p = parseProbeJson(readData(QStringLiteral("samsung_4k60_hlg.json")));
        const QStringList args = buildFfmpegArgs(p, 18'000'000, s, QStringLiteral("in.mp4"), QStringLiteral("out.mp4"));
        QCOMPARE(args.last(), QStringLiteral("out.mp4"));
        QCOMPARE(argAfter(args, QStringLiteral("-i")), QStringLiteral("in.mp4"));
        QCOMPARE(argAfter(args, QStringLiteral("-c:v")), QStringLiteral("libx265"));
        QCOMPARE(argAfter(args, QStringLiteral("-crf")), QStringLiteral("24"));
        QCOMPARE(argAfter(args, QStringLiteral("-preset")), QStringLiteral("slow"));
        QCOMPARE(argAfter(args, QStringLiteral("-tag:v")), QStringLiteral("hvc1"));
        QCOMPARE(argAfter(args, QStringLiteral("-pix_fmt")), QStringLiteral("yuv420p10le"));
        QCOMPARE(argAfter(args, QStringLiteral("-dolbyvision")), QStringLiteral("0"));
        QCOMPARE(argAfter(args, QStringLiteral("-c:a")), QStringLiteral("copy"));
        QCOMPARE(argAfter(args, QStringLiteral("-color_trc")), QStringLiteral("arib-std-b67"));
        const QString x265 = argAfter(args, QStringLiteral("-x265-params"));
        QVERIFY(x265.contains(QStringLiteral("vbv-maxrate=18000")));
        QVERIFY(x265.contains(QStringLiteral("vbv-bufsize=36000")));
        QVERIFY(x265.contains(QStringLiteral("colorprim=bt2020")));
        QVERIFY(x265.contains(QStringLiteral("transfer=arib-std-b67")));
        QVERIFY(x265.contains(QStringLiteral("colormatrix=bt2020nc")));
        QVERIFY(args.contains(QStringLiteral("major_brand=")));
        QVERIFY(std::ranges::any_of(args, [](const QString &a) { return a.startsWith(QStringLiteral("comment=TanzTrans/1")); }));
        QCOMPARE(argAfter(args, QStringLiteral("-map_metadata")), QStringLiteral("0"));
        QCOMPARE(args.count(QStringLiteral("-map")), 2);
        QVERIFY(args.join(QLatin1Char(' ')).contains(QStringLiteral("-map 0:0 -map 0:1")));
    }

    void ffmpegArgsIphone()
    {
        const Settings s;
        const ProbeResult p = parseProbeJson(readData(QStringLiteral("iphone_1080p30_apac.json")));
        const QStringList args = buildFfmpegArgs(p, 4'500'000, s, QStringLiteral("in.mov"), QStringLiteral("out.mp4"));
        QVERIFY(args.join(QLatin1Char(' ')).contains(QStringLiteral("-map 0:2 -map 0:0")));
        QCOMPARE(args.count(QStringLiteral("-map")), 2); // APAC + metadata tracks dropped
        QVERIFY(argAfter(args, QStringLiteral("-x265-params")).contains(QStringLiteral("vbv-maxrate=4500")));
        QCOMPARE(argAfter(args, QStringLiteral("-color_primaries")), QStringLiteral("bt709"));
    }

    void verify()
    {
        const ProbeResult src = parseProbeJson(readData(QStringLiteral("samsung_4k60_hlg.json")));
        ProbeResult out = goodOutput(src);
        VerifyResult v = verifyOutput(src, out, 1000, 400, 25);
        QVERIFY2(v.ok, qPrintable(v.message));

        v = verifyOutput(src, out, 1000, 900, 25);
        QVERIFY(!v.ok);
        QVERIFY(v.noGain);

        out.vtag = QStringLiteral("hev1");
        v = verifyOutput(src, out, 1000, 400, 25);
        QVERIFY(!v.ok);
        QVERIFY(!v.noGain);

        out = goodOutput(src);
        out.duration = src.duration - 3;
        QVERIFY(!verifyOutput(src, out, 1000, 400, 25).ok);

        out = goodOutput(src);
        out.audioStreams = 0;
        QVERIFY(!verifyOutput(src, out, 1000, 400, 25).ok);
    }
};

QTEST_GUILESS_MAIN(TestCore)
#include "tst_core.moc"
