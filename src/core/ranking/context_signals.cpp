#include "core/ranking/context_signals.h"
#include "core/shared/logging.h"

#include <QFileInfo>

namespace bs {

ContextSignals::ContextSignals()
{
    initAppExtensionMap();
}

void ContextSignals::initAppExtensionMap()
{
    // IDE / Code editors
    const QSet<QString> codeExts = {
        QStringLiteral("js"),     QStringLiteral("jsx"),
        QStringLiteral("ts"),     QStringLiteral("tsx"),
        QStringLiteral("py"),     QStringLiteral("cpp"),
        QStringLiteral("c"),      QStringLiteral("h"),
        QStringLiteral("hpp"),    QStringLiteral("swift"),
        QStringLiteral("rs"),     QStringLiteral("go"),
        QStringLiteral("java"),   QStringLiteral("kt"),
        QStringLiteral("rb"),     QStringLiteral("php"),
        QStringLiteral("css"),    QStringLiteral("scss"),
        QStringLiteral("html"),   QStringLiteral("vue"),
        QStringLiteral("json"),   QStringLiteral("yaml"),
        QStringLiteral("yml"),    QStringLiteral("toml"),
        QStringLiteral("xml"),    QStringLiteral("config"),
        QStringLiteral("conf"),   QStringLiteral("ini"),
        QStringLiteral("cmake"),  QStringLiteral("mk"),
        QStringLiteral("proto"),  QStringLiteral("sql"),
    };

    m_appExtensionMap[QStringLiteral("com.microsoft.VSCode")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.microsoft.VSCodeInsiders")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.intellij")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.intellij.ce")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.CLion")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.pycharm")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.pycharm.ce")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.WebStorm")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.GoLand")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.rider")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.rubymine")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.PhpStorm")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.AppCode")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.jetbrains.datagrip")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.sublimetext.4")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.sublimetext.3")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.apple.dt.Xcode")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.panic.Nova")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.github.atom")] = codeExts;
    m_appExtensionMap[QStringLiteral("abnerworks.Typora")] = codeExts;
    m_appExtensionMap[QStringLiteral("com.todesktop.230313mzl4w4u92")] = codeExts; // Cursor
    m_appExtensionMap[QStringLiteral("dev.zed.Zed")] = codeExts;

    // Terminal emulators
    const QSet<QString> termExts = {
        QStringLiteral("sh"),     QStringLiteral("bash"),
        QStringLiteral("zsh"),    QStringLiteral("fish"),
        QStringLiteral("csh"),    QStringLiteral("ksh"),
        QStringLiteral("cfg"),    QStringLiteral("conf"),
        QStringLiteral("config"), QStringLiteral("env"),
        QStringLiteral("rc"),     QStringLiteral("profile"),
        QStringLiteral("log"),    QStringLiteral("txt"),
    };

    m_appExtensionMap[QStringLiteral("com.apple.Terminal")] = termExts;
    m_appExtensionMap[QStringLiteral("com.googlecode.iterm2")] = termExts;
    m_appExtensionMap[QStringLiteral("net.kovidgoyal.kitty")] = termExts;
    m_appExtensionMap[QStringLiteral("co.zeit.hyper")] = termExts;
    m_appExtensionMap[QStringLiteral("com.github.wez.wezterm")] = termExts;
    m_appExtensionMap[QStringLiteral("dev.warp.Warp-Stable")] = termExts;
    m_appExtensionMap[QStringLiteral("com.mitchellh.ghostty")] = termExts;

    // Document viewers/editors
    const QSet<QString> docExts = {
        QStringLiteral("pdf"),    QStringLiteral("docx"),
        QStringLiteral("doc"),    QStringLiteral("txt"),
        QStringLiteral("md"),     QStringLiteral("rtf"),
        QStringLiteral("odt"),    QStringLiteral("pages"),
        QStringLiteral("epub"),   QStringLiteral("tex"),
        QStringLiteral("csv"),    QStringLiteral("xlsx"),
        QStringLiteral("xls"),    QStringLiteral("pptx"),
        QStringLiteral("ppt"),    QStringLiteral("numbers"),
        QStringLiteral("keynote"),
    };

    m_appExtensionMap[QStringLiteral("com.apple.Preview")] = docExts;
    m_appExtensionMap[QStringLiteral("com.microsoft.Word")] = docExts;
    m_appExtensionMap[QStringLiteral("com.microsoft.Excel")] = docExts;
    m_appExtensionMap[QStringLiteral("com.microsoft.Powerpoint")] = docExts;
    m_appExtensionMap[QStringLiteral("com.apple.iWork.Pages")] = docExts;
    m_appExtensionMap[QStringLiteral("com.apple.iWork.Numbers")] = docExts;
    m_appExtensionMap[QStringLiteral("com.apple.iWork.Keynote")] = docExts;
    m_appExtensionMap[QStringLiteral("com.apple.TextEdit")] = docExts;
    m_appExtensionMap[QStringLiteral("net.ia.iaWriter")] = docExts;
    m_appExtensionMap[QStringLiteral("com.ulyssesapp.mac")] = docExts;
    m_appExtensionMap[QStringLiteral("com.google.Chrome")] = docExts;

    // Design tools
    const QSet<QString> designExts = {
        QStringLiteral("png"),    QStringLiteral("jpg"),
        QStringLiteral("jpeg"),   QStringLiteral("gif"),
        QStringLiteral("svg"),    QStringLiteral("webp"),
        QStringLiteral("tiff"),   QStringLiteral("bmp"),
        QStringLiteral("ico"),    QStringLiteral("psd"),
        QStringLiteral("ai"),     QStringLiteral("sketch"),
        QStringLiteral("fig"),    QStringLiteral("xd"),
    };

    m_appExtensionMap[QStringLiteral("com.figma.Desktop")] = designExts;
    m_appExtensionMap[QStringLiteral("com.bohemiancoding.sketch3")] = designExts;
    m_appExtensionMap[QStringLiteral("com.adobe.Photoshop")] = designExts;
    m_appExtensionMap[QStringLiteral("com.adobe.Illustrator")] = designExts;
    m_appExtensionMap[QStringLiteral("com.adobe.InDesign")] = designExts;
    m_appExtensionMap[QStringLiteral("com.pixelmatorteam.pixelmator.x")] = designExts;
    m_appExtensionMap[QStringLiteral("com.apple.Photos")] = designExts;

    // Media players
    const QSet<QString> mediaExts = {
        QStringLiteral("mp4"),    QStringLiteral("mov"),
        QStringLiteral("mkv"),    QStringLiteral("avi"),
        QStringLiteral("wmv"),    QStringLiteral("flv"),
        QStringLiteral("webm"),   QStringLiteral("m4v"),
        QStringLiteral("mp3"),    QStringLiteral("m4a"),
        QStringLiteral("wav"),    QStringLiteral("flac"),
        QStringLiteral("aac"),    QStringLiteral("ogg"),
        QStringLiteral("wma"),    QStringLiteral("aiff"),
    };

    m_appExtensionMap[QStringLiteral("com.apple.QuickTimePlayerX")] = mediaExts;
    m_appExtensionMap[QStringLiteral("org.videolan.vlc")] = mediaExts;
    m_appExtensionMap[QStringLiteral("io.iina.iina")] = mediaExts;
    m_appExtensionMap[QStringLiteral("com.apple.Music")] = mediaExts;
    m_appExtensionMap[QStringLiteral("com.spotify.client")] = mediaExts;
    m_appExtensionMap[QStringLiteral("com.colliderli.iina")] = mediaExts;

    LOG_DEBUG(bsRanking, "initAppExtensionMap: registered %d bundle IDs",
              static_cast<int>(m_appExtensionMap.size()));
}

double ContextSignals::cwdProximityBoost(const QString& filePath, const QString& cwdPath,
                                          int cwdBoostWeight, int maxDepth) const
{
    if (filePath.isEmpty() || cwdPath.isEmpty() || cwdBoostWeight <= 0) {
        return 0.0;
    }

    // Normalize: ensure cwdPath ends with '/'
    QString cwd = cwdPath;
    if (!cwd.endsWith(QLatin1Char('/'))) {
        cwd.append(QLatin1Char('/'));
    }

    // File must be under or at the CWD
    if (!filePath.startsWith(cwd)) {
        // Also check if file IS the cwdPath directory itself
        const QString cwdNoSlash = cwdPath.endsWith(QLatin1Char('/'))
                                       ? cwdPath.left(cwdPath.length() - 1)
                                       : cwdPath;
        if (filePath != cwdNoSlash) {
            return 0.0;
        }
        // File is exactly the CWD directory — depth 0
        return static_cast<double>(cwdBoostWeight);
    }

    // Compute depth: count '/' separators in the relative portion
    const QString relative = filePath.mid(cwd.length());
    const int depth = static_cast<int>(relative.count(QLatin1Char('/')));

    if (depth > maxDepth) {
        return 0.0;
    }

    const double boost = static_cast<double>(cwdBoostWeight) * (1.0 - static_cast<double>(depth) / static_cast<double>(maxDepth + 1));

    LOG_DEBUG(bsRanking, "cwdProximityBoost: file='%s' cwd='%s' depth=%d boost=%.1f",
              qUtf8Printable(filePath), qUtf8Printable(cwdPath), depth, boost);

    return boost;
}

double ContextSignals::appContextBoost(const QString& filePath,
                                        const QString& frontmostAppBundleId,
                                        int appContextBoostWeight) const
{
    if (filePath.isEmpty() || frontmostAppBundleId.isEmpty() || appContextBoostWeight <= 0) {
        return 0.0;
    }

    // Look up the extension set for this bundle ID
    auto it = m_appExtensionMap.find(frontmostAppBundleId);
    if (it == m_appExtensionMap.end()) {
        return 0.0;
    }

    // Extract file extension (without dot, lowered)
    const QFileInfo info(filePath);
    const QString ext = info.suffix().toLower();
    if (ext.isEmpty()) {
        return 0.0;
    }

    if (it.value().contains(ext)) {
        LOG_DEBUG(bsRanking, "appContextBoost: file='%s' app='%s' ext='%s' boost=%d",
                  qUtf8Printable(filePath), qUtf8Printable(frontmostAppBundleId),
                  qUtf8Printable(ext), appContextBoostWeight);
        return static_cast<double>(appContextBoostWeight);
    }

    return 0.0;
}

} // namespace bs
