#include "app/control_plane/control_plane_actor.h"

#include "core/ipc/supervisor.h"
#include "core/shared/logging.h"

#include <QDateTime>

namespace bs {

namespace {

QString normalizedPhase(const QString& phase)
{
    const QString v = phase.trimmed().toLower();
    if (v == QLatin1String("starting")) {
        return QStringLiteral("starting");
    }
    if (v == QLatin1String("running")) {
        return QStringLiteral("running");
    }
    if (v == QLatin1String("shutting_down") || v == QLatin1String("shuttingdown")) {
        return QStringLiteral("shutting_down");
    }
    if (v == QLatin1String("stopped")) {
        return QStringLiteral("stopped");
    }
    return QStringLiteral("starting");
}

AppLifecyclePhase phaseFromString(const QString& phase)
{
    const QString v = normalizedPhase(phase);
    if (v == QLatin1String("running")) {
        return AppLifecyclePhase::Running;
    }
    if (v == QLatin1String("shutting_down")) {
        return AppLifecyclePhase::ShuttingDown;
    }
    if (v == QLatin1String("stopped")) {
        return AppLifecyclePhase::Stopped;
    }
    return AppLifecyclePhase::Starting;
}

} // namespace

ControlPlaneActor::ControlPlaneActor(QObject* parent)
    : QObject(parent)
{
}

ControlPlaneActor::~ControlPlaneActor()
{
    stopAll();
}

void ControlPlaneActor::initialize()
{
    ensureSupervisorInitialized();
}

bool ControlPlaneActor::ensureSupervisorInitialized()
{
    if (m_supervisor) {
        return true;
    }

    m_supervisor = std::make_unique<Supervisor>(this);
    connect(m_supervisor.get(), &Supervisor::serviceStarted,
            this, &ControlPlaneActor::onSupervisorServiceStarted);
    connect(m_supervisor.get(), &Supervisor::serviceStopped,
            this, &ControlPlaneActor::onSupervisorServiceStopped);
    connect(m_supervisor.get(), &Supervisor::serviceCrashed,
            this, &ControlPlaneActor::onSupervisorServiceCrashed);
    connect(m_supervisor.get(), &Supervisor::serviceStateChanged,
            this, &ControlPlaneActor::onSupervisorServiceStateChanged);
    connect(m_supervisor.get(), &Supervisor::allServicesReady,
            this, &ControlPlaneActor::onSupervisorAllReady);
    return true;
}

void ControlPlaneActor::configureServices(const QVariantList& serviceDescriptors)
{
    if (!ensureSupervisorInitialized()) {
        return;
    }

    for (const QVariant& descriptorValue : serviceDescriptors) {
        const QVariantMap descriptor = descriptorValue.toMap();
        const QString name = descriptor.value(QStringLiteral("name")).toString().trimmed();
        const QString binary = descriptor.value(QStringLiteral("binary")).toString().trimmed();
        if (name.isEmpty() || binary.isEmpty()) {
            continue;
        }

        m_supervisor->addService(name, binary);
        m_serviceStates[name] = QStringLiteral("registered");
    }

    m_servicesConfigured = true;
    publishSnapshot();
}

bool ControlPlaneActor::startAll()
{
    if (!ensureSupervisorInitialized()) {
        return false;
    }
    if (!m_servicesConfigured) {
        LOG_WARN(bsCore, "ControlPlaneActor: startAll ignored (services not configured)");
        return false;
    }
    if (m_started && !m_stopping) {
        return true;
    }

    m_stopping = false;
    m_lifecyclePhase = AppLifecyclePhase::Running;
    emit lifecyclePhaseChanged(appLifecyclePhaseToString(m_lifecyclePhase));
    m_started = m_supervisor->startAll();
    publishSnapshot();
    return m_started;
}

void ControlPlaneActor::stopAll()
{
    if (!m_supervisor || m_stopping) {
        return;
    }

    m_stopping = true;
    m_lifecyclePhase = AppLifecyclePhase::ShuttingDown;
    emit lifecyclePhaseChanged(appLifecyclePhaseToString(m_lifecyclePhase));
    m_supervisor->stopAll();
    m_started = false;
    m_stopping = false;
    m_lifecyclePhase = AppLifecyclePhase::Stopped;
    emit lifecyclePhaseChanged(appLifecyclePhaseToString(m_lifecyclePhase));
    publishSnapshot();
}

void ControlPlaneActor::setLifecyclePhase(const QString& phase)
{
    const AppLifecyclePhase next = phaseFromString(phase);
    if (m_lifecyclePhase == next) {
        return;
    }
    m_lifecyclePhase = next;
    emit lifecyclePhaseChanged(appLifecyclePhaseToString(m_lifecyclePhase));
}

QString ControlPlaneActor::lifecyclePhase() const
{
    return appLifecyclePhaseToString(m_lifecyclePhase);
}

QJsonArray ControlPlaneActor::serviceSnapshotSync() const
{
    if (!m_supervisor) {
        return {};
    }
    return m_supervisor->serviceSnapshot();
}

QJsonObject ControlPlaneActor::sendServiceRequestSync(const QString& serviceName,
                                                      const QString& method,
                                                      const QJsonObject& params,
                                                      int timeoutMs)
{
    QJsonObject out;
    out[QStringLiteral("ok")] = false;
    out[QStringLiteral("service")] = serviceName;
    out[QStringLiteral("method")] = method;

    if (!m_supervisor) {
        out[QStringLiteral("error")] = QStringLiteral("supervisor_uninitialized");
        return out;
    }

    SocketClient* client = m_supervisor->clientFor(serviceName);
    if (!client || !client->isConnected()) {
        out[QStringLiteral("error")] = QStringLiteral("service_unavailable");
        return out;
    }

    const auto response = client->sendRequest(method, params, timeoutMs);
    if (!response.has_value()) {
        out[QStringLiteral("error")] = QStringLiteral("request_timeout");
        return out;
    }

    const QString type = response->value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        out[QStringLiteral("error")] =
            response->value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(
                QStringLiteral("request_error"));
        out[QStringLiteral("response")] = *response;
        return out;
    }

    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("response")] = *response;
    return out;
}

void ControlPlaneActor::onSupervisorServiceStarted(const QString& name)
{
    if (m_lifecyclePhase == AppLifecyclePhase::ShuttingDown
        || m_lifecyclePhase == AppLifecyclePhase::Stopped) {
        return;
    }
    updateServiceState(name, QStringLiteral("running"));
}

void ControlPlaneActor::onSupervisorServiceStopped(const QString& name)
{
    updateServiceState(name, QStringLiteral("stopped"));
}

void ControlPlaneActor::onSupervisorServiceCrashed(const QString& name, int crashCount)
{
    updateServiceState(name, QStringLiteral("crashed"));
    emit serviceError(name, QStringLiteral("Service crashed (%1 times)").arg(crashCount));
}

void ControlPlaneActor::onSupervisorServiceStateChanged(const QString& name, const QString& state)
{
    updateServiceState(name, state);
}

void ControlPlaneActor::onSupervisorAllReady()
{
    if (m_lifecyclePhase == AppLifecyclePhase::ShuttingDown
        || m_lifecyclePhase == AppLifecyclePhase::Stopped) {
        return;
    }
    emit allServicesReady();
    publishSnapshot();
}

void ControlPlaneActor::publishSnapshot()
{
    if (!m_supervisor) {
        return;
    }

    QJsonArray snapshot = m_supervisor->serviceSnapshot();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < snapshot.size(); ++i) {
        QJsonObject row = snapshot.at(i).toObject();
        row[QStringLiteral("updatedAtMs")] = now;
        snapshot[i] = row;
    }
    emit managedServicesUpdated(snapshot);
}

void ControlPlaneActor::updateServiceState(const QString& name, const QString& status)
{
    if (name.isEmpty()) {
        return;
    }
    m_serviceStates[name] = status;
    if (m_lifecyclePhase != AppLifecyclePhase::ShuttingDown
        && m_lifecyclePhase != AppLifecyclePhase::Stopped) {
        emit serviceStatusChanged(name, status);
    }
    publishSnapshot();
}

} // namespace bs
