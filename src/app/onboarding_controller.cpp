#include "onboarding_controller.h"

#include "core/shared/fda_check.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

namespace bs {

namespace {

QString settingsDir()
{
    const QString overrideDir = qEnvironmentVariable("BETTERSPOTLIGHT_SETTINGS_DIR").trimmed();
    if (!overrideDir.isEmpty()) {
        return overrideDir;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString settingsPath()
{
    return settingsDir() + QStringLiteral("/settings.json");
}

QJsonObject readSettings()
{
    QFile file(settingsPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object();
}

bool writeSettings(const QJsonObject& obj)
{
    QDir dir;
    if (!dir.mkpath(settingsDir())) {
        qWarning("OnboardingController: failed to create settings directory %s",
                 qPrintable(settingsDir()));
        return false;
    }

    QSaveFile file(settingsPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("OnboardingController: failed to write settings to %s",
                 qPrintable(settingsPath()));
        return false;
    }

    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        qWarning("OnboardingController: failed to write complete settings payload to %s",
                 qPrintable(settingsPath()));
        return false;
    }
    if (!file.commit()) {
        qWarning("OnboardingController: failed to commit settings to %s",
                 qPrintable(settingsPath()));
        return false;
    }
    return true;
}

QString iconForDir(const QString& name)
{
    // Map well-known directories to representative emoji icons
    static const QHash<QString, QString> icons = {
        {QStringLiteral("Documents"),    QStringLiteral("\U0001F4C4")},  // page
        {QStringLiteral("Desktop"),      QStringLiteral("\U0001F5A5")},  // desktop
        {QStringLiteral("Downloads"),    QStringLiteral("\U0001F4E5")},  // inbox tray
        {QStringLiteral("Pictures"),     QStringLiteral("\U0001F5BC")},  // picture
        {QStringLiteral("Music"),        QStringLiteral("\U0001F3B5")},  // note
        {QStringLiteral("Movies"),       QStringLiteral("\U0001F3AC")},  // clapper
        {QStringLiteral("Developer"),    QStringLiteral("\U0001F4BB")},  // laptop
        {QStringLiteral("Projects"),     QStringLiteral("\U0001F4BB")},  // laptop
        {QStringLiteral("Code"),         QStringLiteral("\U0001F4BB")},  // laptop
        {QStringLiteral("Library"),      QStringLiteral("\U00002699")},  // gear
        {QStringLiteral("Applications"), QStringLiteral("\U0001F4E6")},  // package
        {QStringLiteral("Public"),       QStringLiteral("\U0001F310")},  // globe
    };
    return icons.value(name, QStringLiteral("\U0001F4C1"));  // default: folder
}

bool isValidRootMode(const QString& mode)
{
    return mode == QLatin1String("index_embed")
        || mode == QLatin1String("index_only")
        || mode == QLatin1String("skip");
}

QString normalizedRootMode(const QString& rawMode)
{
    const QString mode = rawMode.trimmed();
    return isValidRootMode(mode) ? mode : QStringLiteral("index_only");
}

bool isSafeHomeDirectoryName(const QString& name)
{
    return !name.isEmpty()
        && name != QLatin1String(".")
        && name != QLatin1String("..")
        && !name.contains(QLatin1Char('/'))
        && !name.contains(QLatin1Char('\\'));
}

QString currentAppBundlePath()
{
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.dirName() != QLatin1String("MacOS") || !dir.cdUp()) {
        return {};
    }
    if (dir.dirName() != QLatin1String("Contents") || !dir.cdUp()) {
        return {};
    }
    if (!dir.dirName().endsWith(QStringLiteral(".app"))) {
        return {};
    }
    return QFileInfo(dir.path()).canonicalFilePath();
}

void registerCurrentAppBundleWithLaunchServices()
{
    const QString bundlePath = currentAppBundlePath();
    if (bundlePath.isEmpty()) {
        return;
    }

    const QString lsregister = QStringLiteral(
        "/System/Library/Frameworks/CoreServices.framework/Frameworks/"
        "LaunchServices.framework/Support/lsregister");
    if (!QFileInfo::exists(lsregister)) {
        return;
    }

    QProcess process;
    process.setProgram(lsregister);
    process.setArguments({QStringLiteral("-f"), bundlePath});
    process.start();
    process.waitForFinished(1500);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OnboardingController::OnboardingController(QObject* parent)
    : QObject(parent)
{
    auto settings = readSettings();
    if (settings.value(QStringLiteral("onboarding_completed")).toBool(false)) {
        m_needsOnboarding = false;
    }

    if (m_needsOnboarding) {
        scanHomeDirectories();
    }
}

// ---------------------------------------------------------------------------
// Property accessors
// ---------------------------------------------------------------------------

bool OnboardingController::needsOnboarding() const
{
    return m_needsOnboarding;
}

bool OnboardingController::fdaGranted() const
{
    return m_fdaGranted;
}

QString OnboardingController::fdaStatusMessage() const
{
    return m_fdaStatusMessage;
}

QVariantList OnboardingController::homeDirectories() const
{
    return m_homeDirectories;
}

// ---------------------------------------------------------------------------
// Q_INVOKABLE methods
// ---------------------------------------------------------------------------

bool OnboardingController::refreshFda()
{
    return updateFdaState();
}

bool OnboardingController::checkFda()
{
    const bool hasAccess = updateFdaState();
    setFdaStatusMessage(hasAccess
        ? tr("Access granted")
        : tr("Still not granted"));
    return hasAccess;
}

bool OnboardingController::updateFdaState()
{
    const bool hasAccess = FdaCheck::hasFullDiskAccess();
    if (hasAccess != m_fdaGranted) {
        m_fdaGranted = hasAccess;
        emit fdaGrantedChanged();
    }
    return hasAccess;
}

void OnboardingController::setFdaStatusMessage(const QString& message)
{
    if (message == m_fdaStatusMessage) {
        return;
    }
    m_fdaStatusMessage = message;
    emit fdaStatusMessageChanged();
}

void OnboardingController::openFdaSystemSettings()
{
    // Prime Launch Services and TCC discovery before jumping to settings.
    registerCurrentAppBundleWithLaunchServices();
    refreshFda();
    setFdaStatusMessage(tr("System Settings opened; BetterSpotlight should be listed"));
    QProcess::startDetached(
        QStringLiteral("open"),
        {QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")});
}

void OnboardingController::saveHomeMap(const QVariantList& directories)
{
    auto settings = readSettings();

    QJsonArray homeMap;
    QJsonArray indexRoots;
    const QString home = QDir::homePath();
    for (const auto& entry : directories) {
        auto map = entry.toMap();
        const QString name = map.value(QStringLiteral("name")).toString().trimmed();
        const QString mode = normalizedRootMode(map.value(QStringLiteral("mode")).toString());
        if (!isSafeHomeDirectoryName(name)) {
            continue;
        }

        QJsonObject dirObj;
        dirObj[QStringLiteral("name")] = name;
        dirObj[QStringLiteral("mode")] = mode;
        homeMap.append(dirObj);

        if (mode != QLatin1String("skip")) {
            QJsonObject rootObj;
            rootObj[QStringLiteral("path")] = home + QLatin1Char('/') + name;
            rootObj[QStringLiteral("mode")] = mode;
            indexRoots.append(rootObj);
        }
    }

    settings[QStringLiteral("home_directories")] = homeMap;
    settings[QStringLiteral("indexRoots")] = indexRoots;
    (void)writeSettings(settings);
}

void OnboardingController::completeOnboarding()
{
    auto settings = readSettings();
    const bool wasCompleted =
        !m_needsOnboarding || settings.value(QStringLiteral("onboarding_completed")).toBool(false);
    settings[QStringLiteral("onboarding_completed")] = true;
    (void)writeSettings(settings);

    if (m_needsOnboarding) {
        m_needsOnboarding = false;
        emit needsOnboardingChanged();
    }
    if (!wasCompleted) {
        emit onboardingCompleted();
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void OnboardingController::scanHomeDirectories()
{
    m_homeDirectories.clear();

    QDir home = QDir::home();
    const QStringList entries = home.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& name : entries) {
        QVariantMap dir;
        dir[QStringLiteral("name")]          = name;
        dir[QStringLiteral("icon")]          = iconForDir(name);
        dir[QStringLiteral("suggestedMode")] = suggestMode(name);
        m_homeDirectories.append(dir);
    }

    emit homeDirectoriesChanged();
}

QString OnboardingController::suggestMode(const QString& dirName) const
{
    // Directories whose content should be fully indexed and embedded
    static const QStringList embedDirs = {
        QStringLiteral("Documents"),
        QStringLiteral("Desktop"),
        QStringLiteral("Developer"),
        QStringLiteral("Projects"),
        QStringLiteral("Code"),
    };

    // Directories that should be completely skipped
    static const QStringList skipDirs = {
        QStringLiteral(".ssh"),
        QStringLiteral(".gnupg"),
        QStringLiteral(".aws"),
        QStringLiteral("Library"),
        QStringLiteral(".Trash"),
    };

    // Cloud sync directories — index names but don't extract content
    static const QStringList indexOnlyDirs = {
        QStringLiteral("iCloud Drive"),
        QStringLiteral("iCloud Drive (Archive)"),
        QStringLiteral("Dropbox"),
        QStringLiteral("OneDrive"),
        QStringLiteral("Google Drive"),
    };

    if (embedDirs.contains(dirName)) {
        return QStringLiteral("index_embed");
    }
    if (skipDirs.contains(dirName) || dirName.startsWith(QLatin1Char('.'))) {
        return QStringLiteral("skip");
    }
    if (indexOnlyDirs.contains(dirName)) {
        return QStringLiteral("index_only");
    }

    // Default: index metadata/name only
    return QStringLiteral("index_only");
}

} // namespace bs
