#include "onboarding_controller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>

#include <cerrno>
#include <dirent.h>

namespace bs {

namespace {

QString settingsDir()
{
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

void writeSettings(const QJsonObject& obj)
{
    QDir dir;
    dir.mkpath(settingsDir());

    QFile file(settingsPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("OnboardingController: failed to write settings to %s",
                 qPrintable(settingsPath()));
        return;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
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

bool tryOpenDirectory(const QString& path, bool* accessDenied)
{
    if (accessDenied) {
        *accessDenied = false;
    }

    const QByteArray nativePath = QFile::encodeName(path);
    errno = 0;
    DIR* dir = ::opendir(nativePath.constData());
    if (dir != nullptr) {
        ::closedir(dir);
        return true;
    }

    if (accessDenied && (errno == EACCES || errno == EPERM)) {
        *accessDenied = true;
    }
    return false;
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

QVariantList OnboardingController::homeDirectories() const
{
    return m_homeDirectories;
}

// ---------------------------------------------------------------------------
// Q_INVOKABLE methods
// ---------------------------------------------------------------------------

void OnboardingController::checkFda()
{
    // Probe several protected directories. This both detects FDA and primes
    // System Settings to list the app for manual toggling.
    const QString home = QDir::homePath();
    const QStringList protectedPaths = {
        home + QStringLiteral("/Library/Mail"),
        home + QStringLiteral("/Library/Safari"),
        home + QStringLiteral("/Library/Messages"),
        home + QStringLiteral("/Library/Calendars"),
        home + QStringLiteral("/Library/AddressBook"),
        home + QStringLiteral("/Library/Autosave Information")
    };

    bool accessible = false;
    bool sawDenied = false;
    for (const QString& path : protectedPaths) {
        bool denied = false;
        if (tryOpenDirectory(path, &denied)) {
            accessible = true;
            break;
        }
        sawDenied = sawDenied || denied;
    }

    // Preserve previous heuristic for users with sparse protected directories.
    if (!accessible && !sawDenied) {
        const QString legacyPath = home + QStringLiteral("/Library/Mail");
        QFileInfo info(legacyPath);
        if (info.exists() && info.isReadable()) {
            accessible = true;
        }
    }

    if (accessible != m_fdaGranted) {
        m_fdaGranted = accessible;
        emit fdaGrantedChanged();
    }
}

void OnboardingController::openFdaSystemSettings()
{
    // Prime FDA registration before jumping to settings.
    checkFda();
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
        const QString mode = map.value(QStringLiteral("mode")).toString();
        if (name.isEmpty()) {
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
    if (!indexRoots.isEmpty()) {
        settings[QStringLiteral("indexRoots")] = indexRoots;
    }
    writeSettings(settings);
}

void OnboardingController::completeOnboarding()
{
    auto settings = readSettings();
    const bool wasCompleted = settings.value(QStringLiteral("onboarding_completed")).toBool(false);
    settings[QStringLiteral("onboarding_completed")] = true;
    writeSettings(settings);

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
